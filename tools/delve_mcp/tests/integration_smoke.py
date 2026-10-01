#!/usr/bin/env python3
"""Integration smoke: a real DelveServe process + a raw-socket line-JSON client.

Stdlib only (no mcp, no venv) — ctest ``DelveServe_rpc_py`` runs this against
the just-built binary:

    python3 tools/delve_mcp/tests/integration_smoke.py <DelveServe binary> <repo root>

One TCP connection is kept for the whole run on purpose: it exercises the
server-side per-client "current file" (load sets it, validate falls back to
it). Exit code 0 = all checks passed, 1 otherwise.
"""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import tempfile
import time

HOST = "127.0.0.1"
PROJECT = "src/apps/DelveViewer/smoke_project.json"
CLIENT_FILE_NOTE = "used the client current file"  # kClientFileNote, ServeRuntime.cpp

_failures: list[str] = []


def check(name: str, cond: bool, detail: str = "") -> None:
    tag = "ok" if cond else "FAIL"
    line = f"CHECK {name}: {tag}"
    if not cond and detail:
        line += f" — {detail}"
    print(line, flush=True)
    if not cond:
        _failures.append(name)


def free_port() -> int:
    with socket.socket() as s:
        s.bind((HOST, 0))
        return int(s.getsockname()[1])


def wait_for_port(port: int, timeout_s: float = 30.0) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((HOST, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.2)
    return False


class Client:
    """Minimal line-JSON RPC client; one connection for the whole test."""

    def __init__(self, port: int, timeout: float = 60.0):
        self.sock = socket.create_connection((HOST, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self._buf = b""

    def call(self, op: str, args: dict | None = None) -> dict:
        payload: dict = {"op": op}
        if args:
            payload["args"] = args
        self.sock.sendall((json.dumps(payload) + "\n").encode("utf-8"))
        while b"\n" not in self._buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("DelveServe closed the connection")
            self._buf += chunk
        idx = self._buf.index(b"\n")
        line = self._buf[:idx]
        self._buf = self._buf[idx + 1:]
        return json.loads(line.decode("utf-8"))

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <DelveServe binary> <repo root>", file=sys.stderr)
        return 1
    binary, repo_root = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    if not os.path.isfile(binary):
        print(f"DelveServe binary not found: {binary}", file=sys.stderr)
        return 1

    port = free_port()
    log = tempfile.NamedTemporaryFile(  # pylint: disable=consider-using-with
        mode="w+b", prefix="delve_serve_rpc_py_", suffix=".log", delete=False
    )
    proc = subprocess.Popen(  # noqa: S603 — argv is explicit, no shell
        [binary, "--host", HOST, "--port", str(port)],
        cwd=repo_root,
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    try:
        check("port opens", wait_for_port(port), f"DelveServe did not listen on {port}; log: {log.name}")
        if _failures:
            return 1

        client = Client(port)
        try:
            ping = client.call("ping")
            check("ping ok", ping.get("ok") is True, json.dumps(ping))
            data = ping.get("data") or {}
            check("ping pong", data.get("pong") is True, json.dumps(data))
            check("ping app", data.get("app") == "DelveServe", json.dumps(data))
            check("ping protocol", data.get("protocol") == 1, json.dumps(data))

            status = client.call("status")
            sdata = status.get("data") or {}
            check("status ok", status.get("ok") is True, json.dumps(status))
            check("status port", sdata.get("port") == port, json.dumps(sdata))
            check("status assets_dir", bool(sdata.get("assets_dir")), json.dumps(sdata))

            load = client.call("load", {"path": PROJECT})
            ldata = load.get("data") or {}
            check("load ok", load.get("ok") is True, json.dumps(load))
            check("load has_errors false", ldata.get("has_errors") is False,
                  json.dumps(ldata))
            session = ldata.get("session") or {}
            check("load session file", str(session.get("file", "")).endswith(PROJECT),
                  json.dumps(ldata))

            # No "file" arg: the server must fall back to this client's current
            # file (set by the load above) and say so in the note.
            validate = client.call("validate")
            vdata = validate.get("data") or {}
            check("validate ok", validate.get("ok") is True, json.dumps(validate))
            check("validate has_errors false", vdata.get("has_errors") is False,
                  json.dumps(vdata))
            check("validate clientFile fallback", vdata.get("note") == CLIENT_FILE_NOTE,
                  json.dumps(vdata))
            check("validate session file",
                  str((vdata.get("session") or {}).get("file", "")) == str(session.get("file")),
                  json.dumps(vdata))
        finally:
            client.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)
        log.close()
        if _failures:
            try:
                with open(log.name, "rb") as f:
                    tail = f.read().decode("utf-8", "replace")[-4000:]
                print(f"--- DelveServe log tail ({log.name}) ---\n{tail}", flush=True)
            except OSError:
                pass

    if _failures:
        print(f"FAILED: {len(_failures)} check(s): {', '.join(_failures)}")
        return 1
    print("PASSED: all DelveServe_rpc_py checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
