"""Thin Python client for the DelveServe RPC server (raw TCP + line-JSON).

DelveServe listens on 127.0.0.1:9879 by default (``--port`` / ``--host`` or
env ``DELVE_SERVE_PORT``, see ``src/apps/DelveServe/ServeRpcServer.h``). Each
request is one JSON object terminated by '\\n'; each response is one JSON
object terminated by '\\n'. Replies can be slow: ``fill``/``check``/``export``
run on the CPU worker pool and a cold fill of a big project takes tens of
seconds — hence the generous default timeout.

Adapted copy of thirdparty/pgg/tools/pgg_mcp/rpc_client.py (same wire
protocol; default port 9879 instead of 9878).

Usage:
    from tools.delve_mcp.rpc_client import DelveRpcClient
    c = DelveRpcClient()
    print(c.call(op="ping"))
    print(c.status())
"""

from __future__ import annotations

import json
import socket
import time
from typing import Any


class DelveRpcError(RuntimeError):
    """Raised when the server returns ok=false."""

    def __init__(self, kind: str, message: str):
        super().__init__(f"[{kind}] {message}")
        self.kind = kind
        self.message = message


def port_open(host: str = "127.0.0.1", port: int = 9879, timeout: float = 0.5) -> bool:
    """True when something accepts TCP connections on host:port."""
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


class DelveRpcClient:
    def __init__(self, host: str = "127.0.0.1", port: int = 9879, timeout: float = 600.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self._buf = b""

    # --- low-level ------------------------------------------------------

    def call(self, **kw: Any) -> dict:
        """Send one command and return the parsed JSON response.

        Raises DelveRpcError on ok=false, ConnectionError on transport issues.
        """
        line = (json.dumps(kw) + "\n").encode("utf-8")
        self.sock.sendall(line)
        data = self._readline()
        resp = json.loads(data.decode("utf-8"))
        if not resp.get("ok"):
            err = resp.get("error") or {}
            raise DelveRpcError(err.get("kind", "?"), err.get("message", ""))
        return resp

    def _readline(self) -> bytes:
        # TCP does not guarantee that one recv == one line; accumulate.
        while b"\n" not in self._buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("DelveServe RPC server closed the connection")
            self._buf += chunk
        idx = self._buf.index(b"\n")
        line = self._buf[:idx]
        self._buf = self._buf[idx + 1:]
        return line

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    # --- high-level helpers --------------------------------------------

    def ping(self) -> bool:
        return bool(self.call(op="ping").get("data", {}).get("pong"))

    def status(self) -> dict:
        return self.call(op="status").get("data", {})


def wait_for_port(host: str = "127.0.0.1", port: int = 9879,
                  timeout_s: float = 30.0, step_s: float = 0.5) -> bool:
    """Poll until the RPC port accepts connections (used after auto-start)."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if port_open(host, port):
            return True
        time.sleep(step_s)
    return False


if __name__ == "__main__":
    # Quick smoke when run directly: ping + status.
    c = DelveRpcClient()
    print("ping:", c.ping())
    print("status:", c.status())
    c.close()
