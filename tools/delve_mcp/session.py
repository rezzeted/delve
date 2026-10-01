"""Cross-platform DelveServe lifecycle + RPC proxy for the MCP server.

The MCP process is Python and is the same on every OS. This module finds or
starts ``DelveServe`` and forwards JSON ops. If the binary is missing it
returns a structured ``need_build`` error with configure/build argv for the
current platform — the MCP never runs cmake itself.

Adapted copy of thirdparty/pgg/tools/pgg_mcp/session.py (same lifecycle
pattern; the submodule is not modified). Differences: DelveServe is CPU-only
(no display/xvfb handling), the RPC port comes from env ``DELVE_SERVE_PORT``
(default 9879), the binary override env is ``DELVE_SERVE``, and slot ops are
the delve pipeline ops.

Each in-flight tool call uses its own TCP connection so two FastMCP
invocations cannot mix JSON on one socket. Slot identity is the canonical
project path (optional ``file=`` on ops; the last successful load is attached
as a fallback — a fresh TCP connection has no server-side current file).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Mapping, Optional

from tools.delve_mcp.rpc_client import DelveRpcClient, DelveRpcError, port_open, wait_for_port

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 9879
PORT_ENV = "DELVE_SERVE_PORT"
BINARY_ENV = "DELVE_SERVE"
_SERVE_REL = "src/apps/DelveServe"
_SERVE_NAME = "DelveServe"

# A freshly linked binary younger than this is treated as still being written.
_BINARY_SETTLE_S = 2.0

# Ops that take a project slot via the optional "file" arg.
_SLOT_OPS = frozenset(
    {"validate", "layout", "ir", "fill", "check", "export", "units", "provenance",
     "asset_check"}
)

# DelveServe keeps at most 4 project slots (ServeRuntime::kMaxSlots).
_MAX_REPLAYED_LOADS = 4


def detect_platform(sys_platform: Optional[str] = None) -> str:
    """Map ``sys.platform`` to ``linux`` / ``macos`` / ``windows``."""
    p = (sys_platform or sys.platform).lower()
    if p.startswith("linux"):
        return "linux"
    if p == "darwin":
        return "macos"
    if p.startswith("win") or p.startswith("cygwin"):
        return "windows"
    return "linux"


def _bin(build_dir: str, config: str) -> str:
    return f"{build_dir}/{_SERVE_REL}/{config}/{_SERVE_NAME}"


@dataclass(frozen=True)
class ServeRecipe:
    """Where DelveServe lives and how an agent should build it on this OS."""

    platform: str
    candidates: tuple[str, ...]
    expected: str
    configure: tuple[str, ...]
    build: tuple[str, ...]
    hint: str


def serve_recipe(platform: str) -> ServeRecipe:
    """Canonical search paths + build argv for ``linux`` / ``macos`` / ``windows``."""
    if platform == "windows":
        return ServeRecipe(
            platform="windows",
            candidates=(
                _bin("_intermediate_64", "Debug"),
                _bin("_intermediate_64", "Release"),
            ),
            expected=_bin("_intermediate_64", "Debug") + ".exe",
            configure=("generate_vs.bat",),
            build=("cmake", "--build", "--preset", "debug", "--target", "DelveServe"),
            hint=(
                "Run configure and build from the repo root (generate_vs.bat "
                "configures the vs2022 preset; the release build preset is "
                "'release'). Retry the MCP tool afterwards; it starts DelveServe "
                "itself. Override the binary with env DELVE_SERVE."
            ),
        )
    if platform == "macos":
        return ServeRecipe(
            platform="macos",
            candidates=(
                _bin("_int_clion", "Debug"),
                _bin("_int_clion", "Release"),
                _bin("_int_clion_release", "Release"),
            ),
            expected=_bin("_int_clion", "Debug"),
            configure=("cmake", "--preset", "macos-clion"),
            build=("cmake", "--build", "--preset", "macos-clion-debug", "--target", "DelveServe"),
            hint=(
                "Run configure and build from the repo root (the canonical Debug "
                "build). macos-clion-release is faster for heavy fills. "
                "Retry the MCP tool afterwards; it starts DelveServe itself. "
                "Override the binary with env DELVE_SERVE."
            ),
        )
    return ServeRecipe(
        platform="linux",
        candidates=(
            _bin("_int_linux", "Debug"),
            _bin("_int_linux_release", "Release"),
            _bin("_int_linux", "Release"),
        ),
        expected=_bin("_int_linux", "Debug"),
        configure=("./build_linux.sh",),
        build=("cmake", "--build", "--preset", "linux-debug", "--target", "DelveServe"),
        hint=(
            "build_linux.sh configures the Debug preset (linux). First configure "
            "fetches vcpkg; linux-release is faster for heavy fills. "
            "Retry the MCP tool afterwards; it starts DelveServe itself. "
            "Override the binary with env DELVE_SERVE."
        ),
    )


def _existing_file(path: str) -> Optional[str]:
    for cand in (path, path + ".exe"):
        if os.path.isfile(cand):
            return cand
    return None


def find_serve_binary(
    repo_root: str,
    platform: Optional[str] = None,
    environ: Optional[Mapping[str, str]] = None,
) -> Optional[str]:
    """First existing DelveServe wins. ``DELVE_SERVE`` first, then the candidates."""
    env = environ if environ is not None else os.environ
    plat = platform or detect_platform()
    env_path = env.get(BINARY_ENV)
    if env_path:
        found = _existing_file(env_path)
        if found:
            return found

    recipe = serve_recipe(plat)
    others = [p for p in ("linux", "macos", "windows") if p != plat]
    seen: list[str] = []
    for rel in list(recipe.candidates) + [
        c for p in others for c in serve_recipe(p).candidates
    ]:
        if rel in seen:
            continue
        seen.append(rel)
        abs_path = rel if os.path.isabs(rel) else os.path.join(repo_root, rel)
        found = _existing_file(abs_path)
        if found:
            return found
    return None


def need_build_error(
    repo_root: str,
    platform: Optional[str] = None,
    environ: Optional[Mapping[str, str]] = None,
    *,
    message: Optional[str] = None,
) -> dict[str, Any]:
    """Structured ``ok=false`` envelope: agent should build, then retry.

    ``build`` is the ordered list of steps (configure, then build), each
    ``{"argv": [...], "cwd": <repo root>}`` — ready to run as-is.
    """
    env = environ if environ is not None else os.environ
    plat = platform or detect_platform()
    recipe = serve_recipe(plat)
    env_path = env.get(BINARY_ENV)
    extra = ""
    if env_path and not _existing_file(env_path):
        extra = f" {BINARY_ENV} is set but not a file: {env_path}."
    steps = [
        {"argv": list(recipe.configure), "cwd": repo_root},
        {"argv": list(recipe.build), "cwd": repo_root},
    ]
    return {
        "ok": False,
        "error": {
            "kind": "need_build",
            "message": (
                message
                or (
                    "DelveServe binary not found."
                    + extra
                    + " Build it from the repo root, then retry this tool."
                )
            ),
            "target": "DelveServe",
            "platform": plat,
            "cwd": repo_root,
            "build": steps,
            "expected": recipe.expected,
            "candidates": list(recipe.candidates),
            "hint": recipe.hint,
            "serve": "missing",
        },
    }


def error_envelope(kind: str, message: str, **extra: Any) -> dict[str, Any]:
    err: dict[str, Any] = {"kind": kind, "message": message}
    err.update(extra)
    return {"ok": False, "error": err}


def default_repo_root(environ: Optional[Mapping[str, str]] = None) -> str:
    env = environ if environ is not None else os.environ
    override = env.get("DELVE_REPO_ROOT")
    if override:
        # Same "~" case as launch._repo_root: expand before resolve.
        return str(Path(override).expanduser().resolve())
    return str(Path(__file__).resolve().parent.parent.parent)


def default_port(environ: Optional[Mapping[str, str]] = None) -> int:
    """RPC port: env ``DELVE_SERVE_PORT``, else 9879. Two checkouts need not share a daemon."""
    env = environ if environ is not None else os.environ
    raw = env.get(PORT_ENV, "")
    if raw:
        try:
            port = int(raw)
            if 0 < port < 65536:
                return port
        except ValueError:
            pass
    return DEFAULT_PORT


@dataclass
class DelveSession:
    """Long-lived proxy: auto-start DelveServe, then TCP JSON-RPC."""

    repo_root: str = field(default_factory=default_repo_root)
    host: str = DEFAULT_HOST
    port: Optional[int] = None
    platform: Optional[str] = None
    environ: Optional[Mapping[str, str]] = None
    port_open_fn: Callable[..., bool] = port_open
    wait_for_port_fn: Callable[..., bool] = wait_for_port
    popen_fn: Callable[..., Any] = subprocess.Popen
    client_factory: Optional[Callable[[], DelveRpcClient]] = None
    time_fn: Callable[[], float] = time.time
    mtime_fn: Callable[[str], float] = os.path.getmtime

    _proc: Any = field(default=None, init=False, repr=False)
    _log_file: Any = field(default=None, init=False, repr=False)
    _started_mtime: Optional[float] = field(default=None, init=False, repr=False)
    _loaded: dict[str, dict[str, Any]] = field(default_factory=dict, init=False, repr=False)
    _notes: dict[str, Any] = field(default_factory=dict, init=False, repr=False)
    _foreign_stale: Optional[dict[str, Any]] = field(default=None, init=False, repr=False)
    _foreign_checked_mtime: Optional[float] = field(default=None, init=False, repr=False)
    binary_path: Optional[str] = field(default=None, init=False)
    last_file: Optional[str] = field(default=None, init=False)

    def __post_init__(self) -> None:
        if self.platform is None:
            self.platform = detect_platform()
        if self.environ is None:
            self.environ = os.environ
        if self.port is None:
            self.port = default_port(self.environ)

    def _env(self) -> Mapping[str, str]:
        return self.environ if self.environ is not None else os.environ

    def find_binary(self) -> Optional[str]:
        return find_serve_binary(self.repo_root, self.platform, self._env())

    def _mtime(self, path: Optional[str]) -> Optional[float]:
        if not path:
            return None
        try:
            return self.mtime_fn(path)
        except OSError:
            return None

    def _owns_running_proc(self) -> bool:
        return self._proc is not None and getattr(self._proc, "poll", lambda: 0)() is None

    def _newer_settled_binary(self, since: float) -> Optional[tuple[str, float]]:
        """The binary to run if it was rebuilt after ``since`` and is done linking."""
        serve = self.find_binary()
        mtime = self._mtime(serve)
        if serve is None or mtime is None or mtime <= since:
            return None
        if self.time_fn() - mtime < _BINARY_SETTLE_S:
            return None
        return serve, mtime

    def ensure(self) -> Optional[dict[str, Any]]:
        """Start DelveServe if needed. None = RPC port is accepting."""
        if self.port_open_fn(self.host, self.port):
            if self.binary_path is None:
                self.binary_path = self.find_binary()
            if self._owns_running_proc():
                if self._started_mtime is not None and self._newer_settled_binary(self._started_mtime):
                    return self._restart()
            else:
                self._check_foreign_stale()
            return None

        if self._proc is not None and getattr(self._proc, "poll", lambda: 0)() is None:
            if self.wait_for_port_fn(self.host, self.port, timeout_s=30.0, step_s=0.5):
                return None
            return error_envelope(
                "unreachable",
                "DelveServe did not open the RPC port within 30 s; see tmp/delve_serve.log",
                log="tmp/delve_serve.log",
            )

        serve = self.find_binary()
        if serve is None:
            return need_build_error(self.repo_root, self.platform, self._env())

        # DelveServe is CPU-only: no display/xvfb juggling (unlike PggServe).
        cmd: list[str] = [serve, f"--port={self.port}", f"--host={self.host}"]

        log_dir = Path(self.repo_root) / "tmp"
        log_dir.mkdir(parents=True, exist_ok=True)
        if self._log_file is not None:
            self._log_file.close()
        self._log_file = open(log_dir / "delve_serve.log", "ab", buffering=0)
        popen_kw: dict[str, Any] = {
            "cwd": self.repo_root,
            "stdout": self._log_file,
            "stderr": self._log_file,
        }
        self._proc = self.popen_fn(cmd, **popen_kw)
        self.binary_path = serve
        self._started_mtime = self._mtime(serve)
        if self.wait_for_port_fn(self.host, self.port, timeout_s=30.0, step_s=0.5):
            return None
        poll = getattr(self._proc, "poll", lambda: None)()
        if poll is not None:
            return error_envelope(
                "unreachable",
                f"DelveServe exited early (code {poll}); see tmp/delve_serve.log",
                log="tmp/delve_serve.log",
                binary=serve,
            )
        return error_envelope(
            "unreachable",
            "DelveServe did not open the RPC port within 30 s; see tmp/delve_serve.log",
            log="tmp/delve_serve.log",
            binary=serve,
        )

    def _stop_proc(self) -> None:
        proc = self._proc
        self._proc = None
        if proc is None:
            return
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:  # noqa: BLE001 — a stuck process is killed below
            try:
                proc.kill()
            except Exception:  # noqa: BLE001
                pass
        deadline = self.time_fn() + 10.0
        while self.port_open_fn(self.host, self.port) and self.time_fn() < deadline:
            time.sleep(0.1)

    def _restart(self) -> Optional[dict[str, Any]]:
        """Replace our DelveServe with the rebuilt binary and reload the known slots."""
        self._stop_proc()
        start_error = self.ensure()
        if start_error:
            return start_error
        reloaded: list[str] = []
        failed: dict[str, str] = {}
        keep_last = self.last_file
        for file, load_args in list(self._loaded.items()):
            resp = self._raw_call("load", load_args)
            data = resp.get("data") if resp.get("ok") else None
            # A broken project answers ok=true with has_errors=true (and no slot).
            if isinstance(data, dict) and not data.get("has_errors"):
                reloaded.append(file)
                continue
            msg = (resp.get("error") or {}).get("message")
            if not msg and isinstance(data, dict):
                diags = data.get("diagnostics") or []
                if diags:
                    msg = diags[0].get("message")
            failed[file] = str(msg or "load failed")
        self.last_file = keep_last
        self._notes["restarted"] = True
        self._notes["restart"] = {"binary": self.binary_path, "reloaded_slots": reloaded}
        if failed:
            self._notes["restart"]["failed_slots"] = failed
        return None

    def _check_foreign_stale(self) -> None:
        """Warn when a DelveServe we did not start predates the current build."""
        serve = self.find_binary()
        mtime = self._mtime(serve)
        if mtime is None:
            self._foreign_stale = None
            return
        if self._foreign_stale is None and self._foreign_checked_mtime == mtime:
            return
        self._foreign_checked_mtime = mtime
        resp = self._raw_call("status", {})
        data = resp.get("data") if resp.get("ok") else None
        uptime = data.get("uptime_s") if isinstance(data, dict) else None
        if not isinstance(uptime, (int, float)) or uptime <= 0:
            self._foreign_stale = None
            return
        started = self.time_fn() - float(uptime)
        if mtime > started + 1.0:
            self._foreign_stale = {
                "binary": serve,
                "message": (
                    "DelveServe on the port was started before the last build and not by this MCP; "
                    "it runs the old code. Stop it (the MCP then starts the new binary)."
                ),
            }
        else:
            self._foreign_stale = None

    def _raw_call(self, op: str, args: dict[str, Any]) -> dict[str, Any]:
        payload: dict[str, Any] = {"op": op}
        if args:
            payload["args"] = args
        client: Optional[DelveRpcClient] = None
        try:
            client = self._make_client()
            return client.call(**payload)
        except DelveRpcError as e:
            return error_envelope(e.kind, e.message)
        except (ConnectionError, OSError) as e:
            return error_envelope("unreachable", f"DelveServe RPC unreachable: {e}")
        finally:
            if client is not None:
                client.close()

    def _attach_notes(self, resp: dict[str, Any]) -> dict[str, Any]:
        if self._notes:
            resp.update(self._notes)
            self._notes = {}
        if self._foreign_stale is not None:
            resp["stale_binary"] = dict(self._foreign_stale)
        return resp

    def _make_client(self) -> DelveRpcClient:
        if self.client_factory is not None:
            return self.client_factory()
        return DelveRpcClient(host=self.host, port=self.port)

    def _with_file(self, op: str, args: dict[str, Any]) -> dict[str, Any]:
        if op not in _SLOT_OPS:
            return args
        if args.get("file"):
            return args
        if self.last_file:
            out = dict(args)
            out["file"] = self.last_file
            return out
        return args

    def call(self, op: str, args: Optional[dict[str, Any]] = None) -> dict[str, Any]:
        """Send one RPC op on a fresh TCP connection. Auto-starts DelveServe."""
        start_error = self.ensure()
        if start_error:
            return start_error

        payload_args = {k: v for k, v in (args or {}).items() if v is not None}
        payload_args = self._with_file(op, payload_args)
        payload: dict[str, Any] = {"op": op}
        if payload_args:
            payload["args"] = payload_args

        last_error: Any = None
        for _ in range(2):
            client: Optional[DelveRpcClient] = None
            try:
                client = self._make_client()
                resp = client.call(**payload)
                if op == "load" and resp.get("ok") and isinstance(resp.get("data"), dict):
                    # DelveServe answers load with ok=true even on a broken
                    # project (has_errors=true, no slot created) — only a real
                    # slot becomes the current file and gets replayed.
                    session = resp["data"].get("session") or {}
                    loaded_file = session.get("file")
                    if loaded_file:
                        self.last_file = loaded_file
                        if "path" in payload_args:
                            replay = {"path": payload_args["path"]}
                            self._loaded.pop(loaded_file, None)
                            self._loaded[loaded_file] = replay
                            while len(self._loaded) > _MAX_REPLAYED_LOADS:
                                self._loaded.pop(next(iter(self._loaded)))
                if op == "status" and resp.get("ok") and isinstance(resp.get("data"), dict):
                    data = resp["data"]
                    data["serve"] = "running"
                    if self.binary_path:
                        data["binary"] = self.binary_path
                    data["rpc"] = {"host": self.host, "port": self.port}
                return self._attach_notes(resp)
            except DelveRpcError as e:
                return error_envelope(e.kind, e.message)
            except (ConnectionError, OSError) as e:
                last_error = e
                start_error = self.ensure()
                if start_error:
                    return start_error
            finally:
                if client is not None:
                    client.close()
        return error_envelope(
            "unreachable",
            f"DelveServe RPC unreachable: {last_error}",
        )

    def status(self) -> dict[str, Any]:
        return self.call("status")
