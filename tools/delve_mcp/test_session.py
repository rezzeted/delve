"""Unit tests for tools.delve_mcp.session (no DelveServe process, stdlib only).

Run from the repo root:

    python3 -m unittest tools.delve_mcp.test_session
"""

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.delve_mcp.launch import _repo_root
from tools.delve_mcp.session import (
    DEFAULT_PORT,
    DelveSession,
    default_port,
    default_repo_root,
    detect_platform,
    find_serve_binary,
    need_build_error,
    serve_recipe,
)


def _touch(root: str, rel: str) -> str:
    path = Path(root) / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"")
    return str(path)


class RepoRootTests(unittest.TestCase):
    def test_tilde_repo_root_expands_before_cwd_join(self) -> None:
        home = Path.home()
        raw = "~/sources/delve"
        expected = str((home / "sources" / "delve").resolve())
        self.assertEqual(default_repo_root({"DELVE_REPO_ROOT": raw}), expected)
        # _repo_root() returns the env path only when it exists; give it a fake
        # home so the test does not depend on the host's ~/sources/delve.
        with tempfile.TemporaryDirectory() as tmp:
            fake_home = Path(tmp)
            (fake_home / "sources" / "delve").mkdir(parents=True)
            expected_fake = str((fake_home / "sources" / "delve").resolve())
            env = {
                "DELVE_REPO_ROOT": raw,
                "HOME": str(fake_home),
                "USERPROFILE": str(fake_home),
            }
            with mock.patch.dict(os.environ, env, clear=False):
                self.assertEqual(str(_repo_root()), expected_fake)

    def test_absolute_repo_root_unchanged(self) -> None:
        root = str(Path("/tmp/delve-root").resolve())
        self.assertEqual(default_repo_root({"DELVE_REPO_ROOT": root}), root)

    def test_missing_repo_root_falls_back_to_checkout(self) -> None:
        fallback = Path(__file__).resolve().parent.parent.parent
        with mock.patch.dict(
            os.environ, {"DELVE_REPO_ROOT": "~/no-such-delve-checkout"}, clear=False
        ):
            self.assertEqual(_repo_root(), fallback)


class PlatformTests(unittest.TestCase):
    def test_detect_linux_darwin_win(self) -> None:
        self.assertEqual(detect_platform("linux"), "linux")
        self.assertEqual(detect_platform("linux2"), "linux")
        self.assertEqual(detect_platform("darwin"), "macos")
        self.assertEqual(detect_platform("win32"), "windows")
        self.assertEqual(detect_platform("cygwin"), "windows")

    def test_macos_recipe(self) -> None:
        r = serve_recipe("macos")
        self.assertEqual(r.platform, "macos")
        self.assertEqual(
            r.candidates[0], "_int_clion/src/apps/DelveServe/Debug/DelveServe"
        )
        self.assertEqual(r.expected, "_int_clion/src/apps/DelveServe/Debug/DelveServe")
        self.assertEqual(list(r.configure), ["cmake", "--preset", "macos-clion"])
        self.assertEqual(
            list(r.build),
            ["cmake", "--build", "--preset", "macos-clion-debug", "--target", "DelveServe"],
        )

    def test_windows_recipe(self) -> None:
        r = serve_recipe("windows")
        self.assertEqual(list(r.configure), ["generate_vs.bat"])
        self.assertTrue(r.expected.endswith(".exe"))
        self.assertEqual(
            list(r.build),
            ["cmake", "--build", "--preset", "debug", "--target", "DelveServe"],
        )
        self.assertTrue(r.candidates[0].startswith("_intermediate_64"))

    def test_linux_recipe(self) -> None:
        r = serve_recipe("linux")
        self.assertEqual(list(r.configure), ["./build_linux.sh"])
        self.assertEqual(
            list(r.build),
            ["cmake", "--build", "--preset", "linux-debug", "--target", "DelveServe"],
        )
        self.assertEqual(r.expected, "_int_linux/src/apps/DelveServe/Debug/DelveServe")


class PortTests(unittest.TestCase):
    def test_default_port(self) -> None:
        self.assertEqual(DEFAULT_PORT, 9879)
        self.assertEqual(default_port({}), 9879)

    def test_env_port_override(self) -> None:
        self.assertEqual(default_port({"DELVE_SERVE_PORT": "9999"}), 9999)
        session = DelveSession(
            repo_root="/repo", platform="linux", environ={"DELVE_SERVE_PORT": "9999"}
        )
        self.assertEqual(session.port, 9999)

    def test_invalid_env_port_falls_back(self) -> None:
        self.assertEqual(default_port({"DELVE_SERVE_PORT": "nope"}), 9879)
        self.assertEqual(default_port({"DELVE_SERVE_PORT": "0"}), 9879)
        self.assertEqual(default_port({"DELVE_SERVE_PORT": "70000"}), 9879)

    def test_explicit_port_wins_over_env(self) -> None:
        session = DelveSession(
            repo_root="/repo", platform="linux", port=1234,
            environ={"DELVE_SERVE_PORT": "9999"},
        )
        self.assertEqual(session.port, 1234)


class FindBinaryTests(unittest.TestCase):
    def test_prefers_debug_candidate_order(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            dbg = _touch(root, "_int_linux/src/apps/DelveServe/Debug/DelveServe")
            _touch(root, "_int_linux_release/src/apps/DelveServe/Release/DelveServe")
            found = find_serve_binary(root, platform="linux", environ={})
            self.assertEqual(found, dbg)

    def test_falls_back_to_release(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            rel = _touch(root, "_int_linux_release/src/apps/DelveServe/Release/DelveServe")
            found = find_serve_binary(root, platform="linux", environ={})
            self.assertEqual(found, rel)

    def test_macos_clion_dir(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            dbg = _touch(root, "_int_clion/src/apps/DelveServe/Debug/DelveServe")
            found = find_serve_binary(root, platform="macos", environ={})
            self.assertEqual(found, dbg)

    def test_delve_serve_env_wins(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            _touch(root, "_int_linux/src/apps/DelveServe/Debug/DelveServe")
            custom = _touch(root, "custom/DelveServe")
            found = find_serve_binary(
                root, platform="linux", environ={"DELVE_SERVE": custom}
            )
            self.assertEqual(found, custom)

    def test_stale_env_falls_through_to_preset(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            preset = _touch(root, "_int_linux/src/apps/DelveServe/Debug/DelveServe")
            found = find_serve_binary(
                root,
                platform="linux",
                environ={"DELVE_SERVE": os.path.join(root, "nope", "DelveServe")},
            )
            self.assertEqual(found, preset)

    def test_windows_exe_suffix(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            dbg = _touch(root, "_intermediate_64/src/apps/DelveServe/Debug/DelveServe.exe")
            found = find_serve_binary(root, platform="windows", environ={})
            self.assertEqual(found, dbg)

    def test_missing_returns_none(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            self.assertIsNone(find_serve_binary(root, platform="linux", environ={}))


class NeedBuildTests(unittest.TestCase):
    def test_envelope_shape(self) -> None:
        err = need_build_error("/repo", platform="linux", environ={})
        self.assertFalse(err["ok"])
        e = err["error"]
        self.assertEqual(e["kind"], "need_build")
        self.assertEqual(e["target"], "DelveServe")
        self.assertEqual(e["platform"], "linux")
        self.assertEqual(e["cwd"], "/repo")
        self.assertEqual(e["serve"], "missing")
        build = e["build"]
        self.assertEqual(len(build), 2)
        self.assertEqual(build[0]["argv"], ["./build_linux.sh"])
        self.assertEqual(build[0]["cwd"], "/repo")
        self.assertEqual(
            build[1]["argv"],
            ["cmake", "--build", "--preset", "linux-debug", "--target", "DelveServe"],
        )
        self.assertEqual(build[1]["cwd"], "/repo")
        self.assertIn("expected", e)
        self.assertIn("candidates", e)
        self.assertIn("hint", e)

    def test_macos_build_steps(self) -> None:
        err = need_build_error("/repo", platform="macos", environ={})
        steps = err["error"]["build"]
        self.assertEqual(steps[0]["argv"], ["cmake", "--preset", "macos-clion"])
        self.assertIn("macos-clion-debug", steps[1]["argv"])

    def test_mentions_stale_delve_serve(self) -> None:
        err = need_build_error(
            "/repo",
            platform="linux",
            environ={"DELVE_SERVE": "/nope/DelveServe"},
        )
        self.assertIn("DELVE_SERVE", err["error"]["message"])
        self.assertIn("/nope/DelveServe", err["error"]["message"])


class FakeProc:
    def __init__(self, returncode: int | None = None) -> None:
        self._returncode = returncode

    def poll(self) -> int | None:
        return self._returncode


class SessionEnsureTests(unittest.TestCase):
    def test_port_open_does_not_spawn(self) -> None:
        spawned: list[object] = []

        def popen(*_a: object, **_k: object) -> FakeProc:
            spawned.append(1)
            return FakeProc()

        session = DelveSession(
            repo_root="/repo",
            platform="linux",
            environ={},
            port_open_fn=lambda *_a, **_k: True,
            popen_fn=popen,
        )
        self.assertIsNone(session.ensure())
        self.assertEqual(spawned, [])

    def test_missing_binary_is_need_build(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            session = DelveSession(
                repo_root=root,
                platform="linux",
                environ={},
                port_open_fn=lambda *_a, **_k: False,
                popen_fn=lambda *_a, **_k: FakeProc(),
            )
            err = session.ensure()
            assert err is not None
            self.assertEqual(err["error"]["kind"], "need_build")
            self.assertEqual(err["error"]["target"], "DelveServe")
            self.assertEqual(err["error"]["cwd"], root)

    def test_spawns_serve_when_binary_exists(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            binary = _touch(root, "_int_linux/src/apps/DelveServe/Debug/DelveServe")
            cmds: list[list[str]] = []

            def popen(cmd: list[str], **_k: object) -> FakeProc:
                cmds.append(list(cmd))
                return FakeProc(returncode=None)

            session = DelveSession(
                repo_root=root,
                platform="linux",
                environ={},
                port_open_fn=lambda *_a, **_k: False,
                wait_for_port_fn=lambda *_a, **_k: True,
                popen_fn=popen,
            )
            self.addCleanup(lambda: session._log_file.close() if session._log_file else None)
            self.assertIsNone(session.ensure())
            self.assertEqual(cmds, [[binary, "--port=9879", "--host=127.0.0.1"]])
            self.assertEqual(session.binary_path, binary)

    def test_spawn_uses_env_port(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            binary = _touch(root, "_int_linux/src/apps/DelveServe/Debug/DelveServe")
            cmds: list[list[str]] = []

            def popen(cmd: list[str], **_k: object) -> FakeProc:
                cmds.append(list(cmd))
                return FakeProc(returncode=None)

            session = DelveSession(
                repo_root=root,
                platform="linux",
                environ={"DELVE_SERVE_PORT": "9999"},
                port_open_fn=lambda *_a, **_k: False,
                wait_for_port_fn=lambda *_a, **_k: True,
                popen_fn=popen,
            )
            self.addCleanup(lambda: session._log_file.close() if session._log_file else None)
            self.assertIsNone(session.ensure())
            self.assertEqual(cmds, [[binary, "--port=9999", "--host=127.0.0.1"]])

    def test_call_returns_need_build_without_client(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            session = DelveSession(
                repo_root=root,
                platform="linux",
                environ={},
                port_open_fn=lambda *_a, **_k: False,
            )
            resp = session.call("status")
            self.assertFalse(resp["ok"])
            self.assertEqual(resp["error"]["kind"], "need_build")

    def test_status_enriches_running(self) -> None:
        class FakeClient:
            def call(self, **_kw: object) -> dict:
                return {"ok": True, "data": {"slots": [], "uptime_s": 1}}

            def close(self) -> None:
                pass

        session = DelveSession(
            repo_root="/repo",
            platform="linux",
            environ={},
            port_open_fn=lambda *_a, **_k: True,
            client_factory=FakeClient,
        )
        session.binary_path = "/repo/DelveServe"
        resp = session.status()
        self.assertTrue(resp["ok"])
        self.assertEqual(resp["data"]["serve"], "running")
        self.assertEqual(resp["data"]["binary"], "/repo/DelveServe")
        self.assertEqual(resp["data"]["rpc"]["port"], 9879)

    def test_one_tcp_client_per_call(self) -> None:
        created: list[int] = []
        closed: list[int] = []

        class FakeClient:
            def __init__(self) -> None:
                created.append(1)

            def call(self, **_kw: object) -> dict:
                return {"ok": True, "data": {"pong": True}}

            def close(self) -> None:
                closed.append(1)

        session = DelveSession(
            repo_root="/repo",
            platform="linux",
            environ={},
            port_open_fn=lambda *_a, **_k: True,
            client_factory=FakeClient,
        )
        session.call("ping")
        session.call("ping")
        self.assertEqual(created, [1, 1])
        self.assertEqual(closed, [1, 1])

    def test_injects_last_file_on_slot_ops(self) -> None:
        calls: list[dict] = []

        class FakeClient:
            def call(self, **kw: object) -> dict:
                calls.append(dict(kw))
                if kw.get("op") == "load":
                    return {"ok": True, "data": {"session": {"file": "/repo/a/project.json"}}}
                return {"ok": True, "data": {}}

            def close(self) -> None:
                pass

        session = DelveSession(
            repo_root="/repo",
            platform="linux",
            environ={},
            port_open_fn=lambda *_a, **_k: True,
            client_factory=FakeClient,
        )
        session.call("load", {"path": "a/project.json"})
        session.call("fill", {})
        session.call("fill", {"file": "/other/project.json"})
        session.call("status")  # not a slot op: no file injected
        self.assertEqual(session.last_file, "/repo/a/project.json")
        self.assertEqual(calls[1]["args"]["file"], "/repo/a/project.json")
        self.assertEqual(calls[2]["args"]["file"], "/other/project.json")
        self.assertNotIn("args", calls[3])

    def test_failed_load_keeps_previous_current_file(self) -> None:
        calls: list[dict] = []

        class FakeClient:
            def call(self, **kw: object) -> dict:
                calls.append(dict(kw))
                if kw.get("op") == "load":
                    path = kw["args"]["path"]  # type: ignore[index]
                    if "bad" in str(path):
                        # DelveServe answers ok=true with has_errors and no slot.
                        return {"ok": True, "data": {"file": "/repo/bad.json",
                                                     "diagnostics": [{"code": "D101",
                                                                      "message": "unknown key"}],
                                                     "has_errors": True}}
                    return {"ok": True, "data": {"session": {"file": "/repo/good.json"},
                                                 "has_errors": False}}
                return {"ok": True, "data": {}}

            def close(self) -> None:
                pass

        session = DelveSession(
            repo_root="/repo",
            platform="linux",
            environ={},
            port_open_fn=lambda *_a, **_k: True,
            client_factory=FakeClient,
        )
        session.call("load", {"path": "good.json"})
        session.call("load", {"path": "bad.json"})
        self.assertEqual(session.last_file, "/repo/good.json")
        self.assertEqual(list(session._loaded), ["/repo/good.json"])

    def test_replay_list_capped_at_serve_slots(self) -> None:
        class FakeClient:
            def call(self, **kw: object) -> dict:
                if kw.get("op") == "load":
                    path = kw["args"]["path"]  # type: ignore[index]
                    return {"ok": True, "data": {"session": {"file": "/abs/" + str(path)}}}
                return {"ok": True, "data": {}}

            def close(self) -> None:
                pass

        session = DelveSession(
            repo_root="/repo",
            platform="linux",
            environ={},
            port_open_fn=lambda *_a, **_k: True,
            client_factory=FakeClient,
        )
        for name in ("a", "b", "c", "d", "e"):
            session.call("load", {"path": f"{name}.json"})
        self.assertEqual(
            list(session._loaded),
            ["/abs/b.json", "/abs/c.json", "/abs/d.json", "/abs/e.json"],
        )


class StaleBinaryTests(unittest.TestCase):
    class LiveProc:
        def __init__(self) -> None:
            self.alive = True

        def poll(self) -> int | None:
            return None if self.alive else 0

        def terminate(self) -> None:
            self.alive = False

        def wait(self, timeout: float | None = None) -> int:
            return 0

    def _session(self, root: str, clock: list[float], mtimes: dict[str, float], calls: list[dict],
                 procs: list[object], port: list[bool]) -> DelveSession:
        class FakeClient:
            def call(self, **kw: object) -> dict:
                calls.append(dict(kw))
                if kw.get("op") == "load":
                    path = kw["args"]["path"]  # type: ignore[index]
                    return {"ok": True, "data": {"session": {"file": "/abs/" + str(path)}}}
                if kw.get("op") == "status":
                    return {"ok": True, "data": {"uptime_s": 100.0}}
                return {"ok": True, "data": {}}

            def close(self) -> None:
                pass

        def popen(*_a: object, **_k: object) -> object:
            proc = StaleBinaryTests.LiveProc()
            procs.append(proc)
            port[0] = True
            return proc

        def port_open(*_a: object, **_k: object) -> bool:
            if procs and not getattr(procs[-1], "alive", True):
                port[0] = False
            return port[0]

        session = DelveSession(
            repo_root=root,
            platform="linux",
            environ={},
            port_open_fn=port_open,
            wait_for_port_fn=lambda *_a, **_k: True,
            popen_fn=popen,
            client_factory=FakeClient,
            time_fn=lambda: clock[0],
            mtime_fn=lambda p: mtimes[p],
        )
        self.addCleanup(lambda: session._log_file.close() if session._log_file else None)
        return session

    def test_own_process_restarts_on_rebuilt_binary_and_reloads_slots(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            binary = _touch(root, "_int_linux/src/apps/DelveServe/Debug/DelveServe")
            clock, mtimes = [1000.0], {binary: 900.0}
            calls: list[dict] = []
            procs: list[object] = []
            session = self._session(root, clock, mtimes, calls, procs, [False])
            session.call("load", {"path": "a.json"})
            session.call("load", {"path": "b.json"})
            self.assertEqual(len(procs), 1)
            self.assertNotIn("restarted", session.call("ping"))

            mtimes[binary] = 1001.0
            clock[0] = 1001.5  # still linking: no restart yet
            self.assertNotIn("restarted", session.call("ping"))
            self.assertEqual(len(procs), 1)

            clock[0] = 1010.0
            calls.clear()
            resp = session.call("fill", {})
            self.assertEqual(len(procs), 2)
            self.assertTrue(resp["restarted"])
            self.assertEqual(resp["restart"]["reloaded_slots"], ["/abs/a.json", "/abs/b.json"])
            self.assertEqual([c["op"] for c in calls], ["load", "load", "fill"])
            self.assertEqual(calls[0]["args"], {"path": "a.json"})
            self.assertEqual(calls[2]["args"]["file"], "/abs/b.json")
            self.assertNotIn("restarted", session.call("ping"))

    def test_failed_replay_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            binary = _touch(root, "_int_linux/src/apps/DelveServe/Debug/DelveServe")
            clock, mtimes = [1000.0], {binary: 900.0}
            calls: list[dict] = []
            procs: list[object] = []
            session = self._session(root, clock, mtimes, calls, procs, [False])
            session.call("load", {"path": "a.json"})

            # The project broke on disk while the daemon was down for a rebuild.
            class BrokenClient:
                def call(self, **kw: object) -> dict:
                    calls.append(dict(kw))
                    if kw.get("op") == "load":
                        return {"ok": True, "data": {"file": "/abs/a.json",
                                                     "diagnostics": [{"code": "D101",
                                                                      "message": "unknown key 'x'"}],
                                                     "has_errors": True}}
                    return {"ok": True, "data": {}}

                def close(self) -> None:
                    pass

            mtimes[binary] = 1001.0
            clock[0] = 1010.0
            session.client_factory = BrokenClient
            resp = session.call("ping")
            self.assertTrue(resp["restarted"])
            self.assertEqual(resp["restart"]["reloaded_slots"], [])
            self.assertIn("/abs/a.json", resp["restart"]["failed_slots"])
            self.assertIn("unknown key", resp["restart"]["failed_slots"]["/abs/a.json"])

    def test_foreign_process_gets_stale_binary_warning(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            binary = _touch(root, "_int_linux/src/apps/DelveServe/Debug/DelveServe")
            clock, mtimes = [1000.0], {binary: 950.0}  # server started at 900
            calls: list[dict] = []
            procs: list[object] = []
            session = self._session(root, clock, mtimes, calls, procs, [True])
            resp = session.call("ping")
            self.assertEqual(procs, [])
            self.assertIn("stale_binary", resp)
            self.assertEqual(resp["stale_binary"]["binary"], binary)

            mtimes[binary] = 850.0
            self.assertNotIn("stale_binary", session.call("ping"))


class LaunchModuleTests(unittest.TestCase):
    def test_venv_python_path(self) -> None:
        from tools.delve_mcp.launch import _venv_python

        venv = Path("/tmp/fake-venv")
        with mock.patch("tools.delve_mcp.launch.os.name", "posix"):
            self.assertEqual(_venv_python(venv), venv / "bin" / "python")
        with mock.patch("tools.delve_mcp.launch.os.name", "nt"):
            self.assertEqual(_venv_python(venv), venv / "Scripts" / "python.exe")


if __name__ == "__main__":
    unittest.main()
