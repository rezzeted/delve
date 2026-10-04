"""Cross-platform DelveServe/PggServe lifecycle + RPC proxy for the MCP server.

The MCP process is Python and is the same on every OS. This module finds or
starts the daemon (``DelveServe`` for the pipeline, ``PggServe`` for slot-asset
debugging) and forwards JSON ops. If the binary is missing it returns a
structured ``need_build`` error with configure/build argv for the current
platform — the MCP never runs cmake itself.

Adapted copy of thirdparty/pgg/tools/pgg_mcp/session.py (same lifecycle
pattern; the submodule is not modified). ``DelveSession`` is the CPU-only
delve daemon (no display juggling, port from env ``DELVE_SERVE_PORT``, default
9879); ``PggSession`` is the same machinery flavored for PggServe (GPU — the
xvfb handling is back, port 9878 / ``PGG_SERVE_PORT``, pgg repo root, binary
override ``PGG_SERVE``).

Each in-flight tool call uses its own TCP connection so two FastMCP
invocations cannot mix JSON on one socket. Slot identity is the canonical
file path (optional ``file=`` on ops; the last successful load is attached as
a fallback — a fresh TCP connection has no server-side current file).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, ClassVar, Mapping, Optional

from tools.delve_mcp.rpc_client import DelveRpcClient, DelveRpcError, port_open, wait_for_port

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 9879
PORT_ENV = "DELVE_SERVE_PORT"
BINARY_ENV = "DELVE_SERVE"
_SERVE_REL = "src/apps/DelveServe"
_SERVE_NAME = "DelveServe"

PGG_DEFAULT_PORT = 9878
PGG_PORT_ENV = "PGG_SERVE_PORT"
PGG_BINARY_ENV = "PGG_SERVE"
_PGG_SERVE_REL = "src/apps/PggServe"
_PGG_SERVE_NAME = "PggServe"

# A freshly linked binary younger than this is treated as still being written.
_BINARY_SETTLE_S = 2.0

# Ops that take a session slot via the optional "file" arg.
_SLOT_OPS = frozenset(
    {"validate", "layout", "ir", "fill", "check", "export", "units", "provenance",
     "asset_check"}
)
_PGG_SLOT_OPS = frozenset(
    {"params", "views", "render", "reference", "probe", "export", "diff", "docs"}
)

# DelveServe keeps at most 4 project slots (ServeRuntime::kMaxSlots),
# PggServe — at most 8 (docs/pgg/serve_rpc.md).
_MAX_REPLAYED_LOADS = 4
_PGG_MAX_REPLAYED_LOADS = 8


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


def _bin(build_dir: str, config: str, rel: str = _SERVE_REL, name: str = _SERVE_NAME) -> str:
    return f"{build_dir}/{rel}/{config}/{name}"


def _pgg_bin(build_dir: str, config: str) -> str:
    return _bin(build_dir, config, _PGG_SERVE_REL, _PGG_SERVE_NAME)


@dataclass(frozen=True)
class ServeRecipe:
    """Where the daemon lives and how an agent should build it on this OS."""

    platform: str
    candidates: tuple[str, ...]
    expected: str
    configure: tuple[str, ...]
    build: tuple[str, ...]
    hint: str


def serve_recipe(platform: str) -> ServeRecipe:
    """DelveServe search paths + build argv for ``linux`` / ``macos`` / ``windows``."""
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


def pgg_serve_recipe(platform: str) -> ServeRecipe:
    """PggServe search paths + build argv, relative to the pgg repo root."""
    if platform == "windows":
        return ServeRecipe(
            platform="windows",
            candidates=(
                _pgg_bin("_intermediate_64", "Debug"),
                _pgg_bin("_intermediate_64", "Release"),
            ),
            expected=_pgg_bin("_intermediate_64", "Debug") + ".exe",
            configure=("generate_vs.bat",),
            build=("cmake", "--build", "--preset", "debug", "--target", "PggServe"),
            hint=(
                "Run configure and build from the pgg repo root (thirdparty/pgg): "
                "generate_vs.bat configures the vs2022 preset; the release build "
                "preset is 'release'. Retry the MCP tool afterwards; it starts "
                "PggServe itself. Override the binary with env PGG_SERVE."
            ),
        )
    if platform == "macos":
        return ServeRecipe(
            platform="macos",
            candidates=(
                _pgg_bin("_intermediate_64", "Debug"),
                _pgg_bin("_intermediate_64", "Release"),
                _pgg_bin("_int_clion", "Debug"),
                _pgg_bin("_int_clion_release", "Release"),
            ),
            expected=_pgg_bin("_intermediate_64", "Debug"),
            configure=("./build_mac.sh",),
            build=("cmake", "--build", "--preset", "macos-debug", "--target", "PggServe"),
            hint=(
                "Run configure and build from the pgg repo root (thirdparty/pgg): "
                "build_mac.sh configures the Xcode preset (Debug); macos-release "
                "is faster for heavy renders. Retry the MCP tool afterwards; it "
                "starts PggServe itself. Override the binary with env PGG_SERVE."
            ),
        )
    return ServeRecipe(
        platform="linux",
        candidates=(
            _pgg_bin("_int_linux", "Debug"),
            _pgg_bin("_int_linux_release", "Release"),
            _pgg_bin("_int_linux", "Release"),
        ),
        expected=_pgg_bin("_int_linux", "Debug"),
        configure=("./build_linux.sh",),
        build=("cmake", "--build", "--preset", "linux-debug", "--target", "PggServe"),
        hint=(
            "Run configure and build from the pgg repo root (thirdparty/pgg): "
            "build_linux.sh configures the Debug preset (linux). First configure "
            "fetches vcpkg; linux-release is faster. Headless: PggServe needs a "
            "display (xvfb-run). Retry the MCP tool afterwards; it starts "
            "PggServe itself. Override the binary with env PGG_SERVE."
        ),
    )


def _existing_file(path: str) -> Optional[str]:
    for cand in (path, path + ".exe"):
        if os.path.isfile(cand):
            return cand
    return None


def _find_binary(
    repo_root: str,
    platform: Optional[str],
    environ: Optional[Mapping[str, str]],
    *,
    binary_env: str,
    recipe_fn: Callable[[str], ServeRecipe],
) -> Optional[str]:
    """First existing daemon binary wins: the env override, then the candidates."""
    env = environ if environ is not None else os.environ
    plat = platform or detect_platform()
    env_path = env.get(binary_env)
    if env_path:
        found = _existing_file(env_path)
        if found:
            return found

    recipe = recipe_fn(plat)
    others = [p for p in ("linux", "macos", "windows") if p != plat]
    seen: list[str] = []
    for rel in list(recipe.candidates) + [
        c for p in others for c in recipe_fn(p).candidates
    ]:
        if rel in seen:
            continue
        seen.append(rel)
        abs_path = rel if os.path.isabs(rel) else os.path.join(repo_root, rel)
        found = _existing_file(abs_path)
        if found:
            return found
    return None


def find_serve_binary(
    repo_root: str,
    platform: Optional[str] = None,
    environ: Optional[Mapping[str, str]] = None,
) -> Optional[str]:
    """First existing DelveServe wins. ``DELVE_SERVE`` first, then the candidates."""
    return _find_binary(repo_root, platform, environ,
                        binary_env=BINARY_ENV, recipe_fn=serve_recipe)


def find_pgg_binary(
    repo_root: str,
    platform: Optional[str] = None,
    environ: Optional[Mapping[str, str]] = None,
) -> Optional[str]:
    """First existing PggServe wins. ``PGG_SERVE`` first, then the candidates."""
    return _find_binary(repo_root, platform, environ,
                        binary_env=PGG_BINARY_ENV, recipe_fn=pgg_serve_recipe)


def _need_build_error(
    repo_root: str,
    platform: Optional[str],
    environ: Optional[Mapping[str, str]],
    *,
    app: str,
    binary_env: str,
    recipe_fn: Callable[[str], ServeRecipe],
    message: Optional[str] = None,
) -> dict[str, Any]:
    """Structured ``ok=false`` envelope: agent should build, then retry.

    ``build`` is the ordered list of steps (configure, then build), each
    ``{"argv": [...], "cwd": <repo root>}`` — ready to run as-is.
    """
    env = environ if environ is not None else os.environ
    plat = platform or detect_platform()
    recipe = recipe_fn(plat)
    env_path = env.get(binary_env)
    extra = ""
    if env_path and not _existing_file(env_path):
        extra = f" {binary_env} is set but not a file: {env_path}."
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
                    f"{app} binary not found."
                    + extra
                    + " Build it from the repo root, then retry this tool."
                )
            ),
            "target": app,
            "platform": plat,
            "cwd": repo_root,
            "build": steps,
            "expected": recipe.expected,
            "candidates": list(recipe.candidates),
            "hint": recipe.hint,
            "serve": "missing",
        },
    }


def need_build_error(
    repo_root: str,
    platform: Optional[str] = None,
    environ: Optional[Mapping[str, str]] = None,
    *,
    message: Optional[str] = None,
) -> dict[str, Any]:
    """DelveServe flavor of the need_build envelope (cwd = delve repo root)."""
    return _need_build_error(repo_root, platform, environ,
                             app=_SERVE_NAME, binary_env=BINARY_ENV,
                             recipe_fn=serve_recipe, message=message)


def pgg_need_build_error(
    repo_root: str,
    platform: Optional[str] = None,
    environ: Optional[Mapping[str, str]] = None,
    *,
    message: Optional[str] = None,
) -> dict[str, Any]:
    """PggServe flavor of the need_build envelope (cwd = pgg repo root)."""
    return _need_build_error(repo_root, platform, environ,
                             app=_PGG_SERVE_NAME, binary_env=PGG_BINARY_ENV,
                             recipe_fn=pgg_serve_recipe, message=message)


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


def pgg_repo_root(environ: Optional[Mapping[str, str]] = None) -> str:
    """PggServe repo root: ``PGG_REPO_ROOT`` (``~`` expanded), else thirdparty/pgg."""
    env = environ if environ is not None else os.environ
    override = env.get("PGG_REPO_ROOT")
    if override:
        return str(Path(override).expanduser().resolve())
    return str(Path(default_repo_root(env)) / "thirdparty" / "pgg")


def _env_port(env_name: str, default: int, environ: Optional[Mapping[str, str]]) -> int:
    """RPC port from an env var; invalid/out-of-range values fall back to the default."""
    env = environ if environ is not None else os.environ
    raw = env.get(env_name, "")
    if raw:
        try:
            port = int(raw)
            if 0 < port < 65536:
                return port
        except ValueError:
            pass
    return default


def default_port(environ: Optional[Mapping[str, str]] = None) -> int:
    """DelveServe port: env ``DELVE_SERVE_PORT``, else 9879."""
    return _env_port(PORT_ENV, DEFAULT_PORT, environ)


def pgg_default_port(environ: Optional[Mapping[str, str]] = None) -> int:
    """PggServe port: env ``PGG_SERVE_PORT``, else 9878."""
    return _env_port(PGG_PORT_ENV, PGG_DEFAULT_PORT, environ)


@dataclass
class DelveSession:
    """Long-lived proxy: auto-start the daemon, then TCP JSON-RPC.

    The daemon identity lives in the ClassVar hooks — ``PggSession`` below is
    the same session flavored for PggServe (override the hooks, not the code).
    """

    APP: ClassVar[str] = _SERVE_NAME
    DEFAULT_PORT: ClassVar[int] = DEFAULT_PORT
    PORT_ENV: ClassVar[str] = PORT_ENV
    BINARY_ENV: ClassVar[str] = BINARY_ENV
    LOG_NAME: ClassVar[str] = "delve_serve.log"
    MAX_REPLAYED_LOADS: ClassVar[int] = _MAX_REPLAYED_LOADS
    SLOT_OPS: ClassVar[frozenset] = _SLOT_OPS
    REPLAY_KEYS: ClassVar[tuple[str, ...]] = ("path",)
    # DelveServe is CPU-only; PggServe renders on the GPU and wants a display.
    NEEDS_DISPLAY: ClassVar[bool] = False
    # Extra argv for the daemon on Linux (PggServe: "--headless" — GLX pbuffer
    # instead of the tiny window; the display connection is still required).
    LINUX_EXTRA_ARGS: ClassVar[tuple[str, ...]] = ()

    repo_root: str = field(default_factory=default_repo_root)
    host: str = DEFAULT_HOST
    port: Optional[int] = None
    platform: Optional[str] = None
    environ: Optional[Mapping[str, str]] = None
    port_open_fn: Callable[..., bool] = port_open
    wait_for_port_fn: Callable[..., bool] = wait_for_port
    popen_fn: Callable[..., Any] = subprocess.Popen
    which_fn: Callable[[str], Optional[str]] = shutil.which
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

    @classmethod
    def recipe(cls, platform: str) -> ServeRecipe:
        return serve_recipe(platform)

    def __post_init__(self) -> None:
        if self.platform is None:
            self.platform = detect_platform()
        if self.environ is None:
            self.environ = os.environ
        if self.port is None:
            self.port = _env_port(self.PORT_ENV, self.DEFAULT_PORT, self.environ)

    def _env(self) -> Mapping[str, str]:
        return self.environ if self.environ is not None else os.environ

    def find_binary(self) -> Optional[str]:
        return _find_binary(self.repo_root, self.platform, self._env(),
                            binary_env=self.BINARY_ENV, recipe_fn=type(self).recipe)

    def _need_build(self) -> dict[str, Any]:
        return _need_build_error(self.repo_root, self.platform, self._env(),
                                 app=self.APP, binary_env=self.BINARY_ENV,
                                 recipe_fn=type(self).recipe)

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
        """Start the daemon if needed. None = RPC port is accepting."""
        log_ref = f"tmp/{self.LOG_NAME}"
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
                f"{self.APP} did not open the RPC port within 30 s; see {log_ref}",
                log=log_ref,
            )

        serve = self.find_binary()
        if serve is None:
            return self._need_build()

        env = self._env()
        cmd: list[str] = [serve, f"--port={self.port}", f"--host={self.host}"]
        if self.platform == "linux":
            cmd.extend(self.LINUX_EXTRA_ARGS)
        if self.NEEDS_DISPLAY and self.platform == "linux" and not env.get("DISPLAY"):
            xvfb = self.which_fn("xvfb-run")
            if xvfb:
                cmd = [xvfb, "-a"] + cmd
            else:
                return error_envelope(
                    "unreachable",
                    f"{self.APP} needs a display: DISPLAY is unset and xvfb-run is not on PATH",
                    hint="Install xvfb (Debian/Ubuntu: xvfb) or run under a graphical session, then retry.",
                    log=log_ref,
                )

        log_dir = Path(self.repo_root) / "tmp"
        log_dir.mkdir(parents=True, exist_ok=True)
        if self._log_file is not None:
            self._log_file.close()
        self._log_file = open(log_dir / self.LOG_NAME, "ab", buffering=0)
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
                f"{self.APP} exited early (code {poll}); see {log_ref}",
                log=log_ref,
                binary=serve,
            )
        return error_envelope(
            "unreachable",
            f"{self.APP} did not open the RPC port within 30 s; see {log_ref}",
            log=log_ref,
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
        """Replace our daemon with the rebuilt binary and reload the known slots."""
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
            # A broken file answers ok=true with has_errors=true (and no slot).
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
        """Warn when a daemon we did not start predates the current build."""
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
                    f"{self.APP} on the port was started before the last build and not by this MCP; "
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
            return error_envelope("unreachable", f"{self.APP} RPC unreachable: {e}")
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
        if op not in self.SLOT_OPS:
            return args
        if args.get("file"):
            return args
        if self.last_file:
            out = dict(args)
            out["file"] = self.last_file
            return out
        return args

    def call(self, op: str, args: Optional[dict[str, Any]] = None) -> dict[str, Any]:
        """Send one RPC op on a fresh TCP connection. Auto-starts the daemon."""
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
                    # Both daemons answer load with ok=true even on a broken
                    # file (has_errors=true, no slot created) — only a real
                    # slot becomes the current file and gets replayed.
                    session = resp["data"].get("session") or {}
                    loaded_file = session.get("file")
                    if loaded_file:
                        self.last_file = loaded_file
                        if "path" in payload_args:
                            replay = {k: v for k, v in payload_args.items()
                                      if k in self.REPLAY_KEYS}
                            self._loaded.pop(loaded_file, None)
                            self._loaded[loaded_file] = replay
                            while len(self._loaded) > self.MAX_REPLAYED_LOADS:
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
            f"{self.APP} RPC unreachable: {last_error}",
        )

    def status(self) -> dict[str, Any]:
        return self.call("status")


@dataclass
class PggSession(DelveSession):
    """PggServe flavor: GPU daemon (--headless on Linux: pbuffer, no window; xvfb
    only when DISPLAY is unset), pgg repo root, port 9878."""

    APP: ClassVar[str] = _PGG_SERVE_NAME
    DEFAULT_PORT: ClassVar[int] = PGG_DEFAULT_PORT
    PORT_ENV: ClassVar[str] = PGG_PORT_ENV
    BINARY_ENV: ClassVar[str] = PGG_BINARY_ENV
    LOG_NAME: ClassVar[str] = "pgg_serve.log"
    MAX_REPLAYED_LOADS: ClassVar[int] = _PGG_MAX_REPLAYED_LOADS
    SLOT_OPS: ClassVar[frozenset] = _PGG_SLOT_OPS
    REPLAY_KEYS: ClassVar[tuple[str, ...]] = ("path", "lib_roots")
    NEEDS_DISPLAY: ClassVar[bool] = True
    LINUX_EXTRA_ARGS: ClassVar[tuple[str, ...]] = ("--headless",)

    repo_root: str = field(default_factory=pgg_repo_root)

    @classmethod
    def recipe(cls, platform: str) -> ServeRecipe:
        return pgg_serve_recipe(platform)
