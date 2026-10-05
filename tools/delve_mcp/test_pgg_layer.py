"""Unit tests for the pgg layer: PggSession flavor + PggLayer lib_roots.

Stdlib only, no PggServe process, no mcp import. Run from the repo root:

    python3 -m unittest tools.delve_mcp.test_pgg_layer
"""

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

from tools.delve_mcp.pgg_layer import PggLayer
from tools.delve_mcp.session import (
    DelveSession,
    PggSession,
    find_pgg_binary,
    pgg_default_port,
    pgg_need_build_error,
    pgg_repo_root,
    pgg_serve_recipe,
)


def _touch(root: str, rel: str, content: bytes = b"") -> str:
    path = Path(root) / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    return str(path)


class FakeProc:
    def __init__(self, returncode: int | None = None) -> None:
        self._returncode = returncode

    def poll(self) -> int | None:
        return self._returncode


class PggRecipeTests(unittest.TestCase):
    def test_macos_recipe(self) -> None:
        r = pgg_serve_recipe("macos")
        self.assertEqual(
            r.candidates[0], "_intermediate_64/src/apps/PggServe/Debug/PggServe"
        )
        self.assertIn("_int_clion/src/apps/PggServe/Debug/PggServe", r.candidates)
        self.assertEqual(r.expected, "_intermediate_64/src/apps/PggServe/Debug/PggServe")
        self.assertEqual(list(r.configure), ["./build_mac.sh"])
        self.assertEqual(
            list(r.build),
            ["cmake", "--build", "--preset", "macos-debug", "--target", "PggServe"],
        )

    def test_windows_recipe(self) -> None:
        r = pgg_serve_recipe("windows")
        self.assertEqual(list(r.configure), ["generate_vs.bat"])
        self.assertTrue(r.expected.endswith(".exe"))
        self.assertEqual(
            list(r.build),
            ["cmake", "--build", "--preset", "debug", "--target", "PggServe"],
        )

    def test_linux_recipe(self) -> None:
        r = pgg_serve_recipe("linux")
        self.assertEqual(list(r.configure), ["./build_linux.sh"])
        self.assertEqual(
            list(r.build),
            ["cmake", "--build", "--preset", "linux-debug", "--target", "PggServe"],
        )
        self.assertEqual(r.expected, "_int_linux/src/apps/PggServe/Debug/PggServe")


class PggRepoRootTests(unittest.TestCase):
    def test_default_is_thirdparty_pgg(self) -> None:
        self.assertEqual(
            pgg_repo_root({"DELVE_REPO_ROOT": "/x/delve"}),
            str(Path("/x/delve/thirdparty/pgg")),
        )

    def test_env_override_expands_tilde(self) -> None:
        home = Path.home()
        self.assertEqual(
            pgg_repo_root({"PGG_REPO_ROOT": "~/sources/pgg"}),
            str((home / "sources" / "pgg").resolve()),
        )


class PggPortTests(unittest.TestCase):
    def test_default_and_env(self) -> None:
        self.assertEqual(pgg_default_port({}), 9878)
        self.assertEqual(pgg_default_port({"PGG_SERVE_PORT": "9990"}), 9990)
        self.assertEqual(pgg_default_port({"PGG_SERVE_PORT": "nope"}), 9878)
        self.assertEqual(pgg_default_port({"PGG_SERVE_PORT": "0"}), 9878)

    def test_session_port_from_env(self) -> None:
        session = PggSession(
            repo_root="/pgg", platform="linux", environ={"PGG_SERVE_PORT": "9990"}
        )
        self.assertEqual(session.port, 9990)


class PggFindBinaryTests(unittest.TestCase):
    def test_debug_first(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            dbg = _touch(root, "_int_linux/src/apps/PggServe/Debug/PggServe")
            _touch(root, "_int_linux_release/src/apps/PggServe/Release/PggServe")
            self.assertEqual(find_pgg_binary(root, platform="linux", environ={}), dbg)

    def test_pgg_serve_env_wins(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            _touch(root, "_int_linux/src/apps/PggServe/Debug/PggServe")
            custom = _touch(root, "custom/PggServe")
            found = find_pgg_binary(root, platform="linux",
                                    environ={"PGG_SERVE": custom})
            self.assertEqual(found, custom)

    def test_macos_intermediate64(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            dbg = _touch(root, "_intermediate_64/src/apps/PggServe/Debug/PggServe")
            self.assertEqual(find_pgg_binary(root, platform="macos", environ={}), dbg)

    def test_windows_exe_suffix(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            dbg = _touch(root, "_intermediate_64/src/apps/PggServe/Debug/PggServe.exe")
            self.assertEqual(find_pgg_binary(root, platform="windows", environ={}), dbg)

    def test_missing_returns_none(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            self.assertIsNone(find_pgg_binary(root, platform="linux", environ={}))


class PggNeedBuildTests(unittest.TestCase):
    def test_envelope_shape(self) -> None:
        err = pgg_need_build_error("/x/delve/thirdparty/pgg", platform="linux", environ={})
        self.assertFalse(err["ok"])
        e = err["error"]
        self.assertEqual(e["kind"], "need_build")
        self.assertEqual(e["target"], "PggServe")
        self.assertEqual(e["platform"], "linux")
        self.assertEqual(e["cwd"], "/x/delve/thirdparty/pgg")
        self.assertEqual(e["serve"], "missing")
        self.assertEqual(e["build"][0], {"argv": ["./build_linux.sh"],
                                         "cwd": "/x/delve/thirdparty/pgg"})
        self.assertEqual(e["build"][1]["argv"],
                         ["cmake", "--build", "--preset", "linux-debug", "--target", "PggServe"])
        self.assertEqual(e["build"][1]["cwd"], "/x/delve/thirdparty/pgg")
        self.assertIn("PggServe", e["message"])

    def test_mentions_stale_pgg_serve_env(self) -> None:
        err = pgg_need_build_error("/pgg", platform="linux",
                                   environ={"PGG_SERVE": "/nope/PggServe"})
        self.assertIn("PGG_SERVE", err["error"]["message"])


class PggSessionEnsureTests(unittest.TestCase):
    def _session(self, root: str, cmds: list[list[str]], **kw: object) -> PggSession:
        def popen(cmd: list[str], **_k: object) -> FakeProc:
            cmds.append(list(cmd))
            return FakeProc(returncode=None)

        session = PggSession(
            repo_root=root,
            port_open_fn=lambda *_a, **_k: False,
            wait_for_port_fn=lambda *_a, **_k: True,
            popen_fn=popen,
            **kw,
        )
        self.addCleanup(lambda: session._log_file.close() if session._log_file else None)
        return session

    def test_spawn_with_display(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            binary = _touch(root, "_int_linux/src/apps/PggServe/Debug/PggServe")
            cmds: list[list[str]] = []
            session = self._session(root, cmds, platform="linux",
                                    environ={"DISPLAY": ":0"})
            self.assertIsNone(session.ensure())
            self.assertEqual(cmds, [[binary, "--port=9878", "--host=127.0.0.1", "--headless"]])

    def test_linux_headless_uses_xvfb(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            binary = _touch(root, "_int_linux/src/apps/PggServe/Debug/PggServe")
            cmds: list[list[str]] = []
            session = self._session(root, cmds, platform="linux", environ={},
                                    which_fn=lambda _name: "/usr/bin/xvfb-run")
            self.assertIsNone(session.ensure())
            self.assertEqual(cmds, [["/usr/bin/xvfb-run", "-a", binary,
                                     "--port=9878", "--host=127.0.0.1", "--headless"]])

    def test_linux_headless_without_xvfb_is_unreachable(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            _touch(root, "_int_linux/src/apps/PggServe/Debug/PggServe")
            spawned: list[object] = []
            session = PggSession(
                repo_root=root,
                platform="linux",
                environ={},
                port_open_fn=lambda *_a, **_k: False,
                popen_fn=lambda *_a, **_k: spawned.append(1) or FakeProc(),
                which_fn=lambda _name: None,
            )
            err = session.ensure()
            assert err is not None
            self.assertEqual(err["error"]["kind"], "unreachable")
            self.assertIn("xvfb-run", err["error"]["message"])
            self.assertIn("hint", err["error"])
            self.assertEqual(spawned, [])

    def test_macos_headless_needs_no_xvfb(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            binary = _touch(root, "_intermediate_64/src/apps/PggServe/Debug/PggServe")
            cmds: list[list[str]] = []
            session = self._session(root, cmds, platform="macos", environ={})
            self.assertIsNone(session.ensure())
            self.assertEqual(cmds, [[binary, "--port=9878", "--host=127.0.0.1"]])

    def test_delve_session_never_needs_display(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            binary = _touch(root, "_int_linux/src/apps/DelveServe/Debug/DelveServe")
            cmds: list[list[str]] = []

            def popen(cmd: list[str], **_k: object) -> FakeProc:
                cmds.append(list(cmd))
                return FakeProc(returncode=None)

            session = DelveSession(
                repo_root=root,
                platform="linux",
                environ={},  # headless: DelveServe is CPU-only, no xvfb
                port_open_fn=lambda *_a, **_k: False,
                wait_for_port_fn=lambda *_a, **_k: True,
                popen_fn=popen,
                which_fn=lambda _name: None,
            )
            self.addCleanup(lambda: session._log_file.close() if session._log_file else None)
            self.assertIsNone(session.ensure())
            self.assertEqual(cmds, [[binary, "--port=9879", "--host=127.0.0.1"]])

    def test_missing_binary_is_need_build(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            session = PggSession(
                repo_root=root,
                platform="linux",
                environ={},
                port_open_fn=lambda *_a, **_k: False,
            )
            resp = session.status()
            self.assertFalse(resp["ok"])
            self.assertEqual(resp["error"]["kind"], "need_build")
            self.assertEqual(resp["error"]["target"], "PggServe")
            self.assertEqual(resp["error"]["cwd"], root)
            argv = resp["error"]["build"][1]["argv"]
            self.assertIn("PggServe", argv)
            self.assertIn("linux-debug", argv)

    def test_replay_keeps_lib_roots(self) -> None:
        class FakeClient:
            def call(self, **kw: object) -> dict:
                if kw.get("op") == "load":
                    path = kw["args"]["path"]  # type: ignore[index]
                    return {"ok": True, "data": {"session": {"file": "/abs/" + str(path)}}}
                return {"ok": True, "data": {}}

            def close(self) -> None:
                pass

        session = PggSession(
            repo_root="/pgg",
            platform="linux",
            environ={},
            port_open_fn=lambda *_a, **_k: True,
            client_factory=FakeClient,
        )
        session.call("load", {"path": "a.pgg", "lib_roots": ["/lib", "/assets"]})
        self.assertEqual(session._loaded["/abs/a.pgg"],
                         {"path": "a.pgg", "lib_roots": ["/lib", "/assets"]})

    def test_injects_last_file_on_render(self) -> None:
        calls: list[dict] = []

        class FakeClient:
            def call(self, **kw: object) -> dict:
                calls.append(dict(kw))
                if kw.get("op") == "load":
                    return {"ok": True, "data": {"session": {"file": "/abs/a.pgg"}}}
                return {"ok": True, "data": {}}

            def close(self) -> None:
                pass

        session = PggSession(
            repo_root="/pgg",
            platform="linux",
            environ={},
            port_open_fn=lambda *_a, **_k: True,
            client_factory=FakeClient,
        )
        session.call("load", {"path": "a.pgg", "lib_roots": []})
        session.call("render", {"node": "facing"})
        self.assertEqual(calls[-1]["args"]["file"], "/abs/a.pgg")


def _fake_pgg_session(calls: list[dict], created: list[int]) -> PggSession:
    class FakeClient:
        def call(self, **kw: object) -> dict:
            calls.append(dict(kw))
            if kw.get("op") == "load":
                path = kw["args"]["path"]  # type: ignore[index]
                return {"ok": True, "data": {"session": {"file": str(path)},
                                             "has_errors": False}}
            return {"ok": True, "data": {}}

        def close(self) -> None:
            pass

    session = PggSession(
        repo_root="/pgg",
        platform="linux",
        environ={},
        port_open_fn=lambda *_a, **_k: True,
        client_factory=FakeClient,
    )
    created.append(1)
    return session


class PggLayerLazyTests(unittest.TestCase):
    def test_pgg_not_created_until_first_pgg_call(self) -> None:
        created: list[int] = []
        pgg_calls: list[dict] = []

        class DelveClient:
            def call(self, **kw: object) -> dict:
                if kw.get("op") == "load":
                    return {"ok": True, "data": {"session": {"file": "/abs/project.json"}}}
                return {"ok": True, "data": {"slots": []}}

            def close(self) -> None:
                pass

        delve = DelveSession(
            repo_root="/repo",
            platform="linux",
            environ={},
            port_open_fn=lambda *_a, **_k: True,
            client_factory=DelveClient,
        )
        layer = PggLayer(delve, pgg_factory=lambda: _fake_pgg_session(pgg_calls, created))

        delve.status()
        delve.call("load", {"path": "project.json"})
        delve.call("fill", {})
        self.assertEqual(created, [])  # delve work never touches the pgg side

        layer.status()
        self.assertEqual(created, [1])
        layer.status()
        self.assertEqual(created, [1])  # created once, then reused


class PggLayerLibRootsTests(unittest.TestCase):
    def _layer(self, root: str, calls: list[dict]) -> PggLayer:
        delve = DelveSession(repo_root=root, platform="linux", environ={})
        return PggLayer(delve, pgg_factory=lambda: _fake_pgg_session(calls, []))

    def test_auto_roots_without_project(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            asset = _touch(root, "assets/walls/facing_v1.pgg")
            calls: list[dict] = []
            layer = self._layer(root, calls)
            resp = layer.load("assets/walls/facing_v1.pgg")
            self.assertTrue(resp["ok"])
            args = calls[0]["args"]
            self.assertEqual(args["path"], asset)  # absolutized against the delve repo
            self.assertEqual(
                args["lib_roots"],
                [str(Path(root) / "assets" / "walls"), str(Path(root) / "assets")],
            )

    def test_relative_path_outside_delve_repo_passes_through(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            calls: list[dict] = []
            layer = self._layer(root, calls)
            layer.load("resources/AmberEstate/cottage.pgg")  # pgg-native relative path
            args = calls[0]["args"]
            self.assertEqual(args["path"], "resources/AmberEstate/cottage.pgg")
            # No asset dir for an unresolved path; the delve assets root stays.
            self.assertEqual(args["lib_roots"], [str(Path(root) / "assets")])

    def test_explicit_lib_roots_absolutized(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            asset = _touch(root, "assets/walls/facing_v1.pgg")
            calls: list[dict] = []
            layer = self._layer(root, calls)
            layer.load("assets/walls/facing_v1.pgg", lib_roots=["assets", "/abs/lib"])
            args = calls[0]["args"]
            self.assertEqual(args["path"], asset)
            self.assertEqual(args["lib_roots"],
                             [str(Path(root) / "assets"), "/abs/lib"])

    def _write_project(self, root: str, body: str, mtime: float) -> str:
        proj = Path(root) / "proj" / "project.json"
        proj.parent.mkdir(parents=True, exist_ok=True)
        proj.write_text(body, encoding="utf-8")
        os.utime(proj, (mtime, mtime))
        return str(proj)

    def test_project_roots_from_loaded_delve_project(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            _touch(root, "assets/walls/facing_v1.pgg")
            proj = self._write_project(root, '{"asset_roots": ["assets", "../shared"]}', 1000)
            calls: list[dict] = []
            layer = self._layer(root, calls)
            layer.delve.last_file = proj
            layer.load("assets/walls/facing_v1.pgg")
            roots = calls[0]["args"]["lib_roots"]
            pdir = str(Path(root) / "proj")
            self.assertEqual(
                roots,
                [str(Path(root) / "assets" / "walls"),
                 str(Path(root) / "assets"),
                 pdir,
                 str(Path(pdir) / "assets"),
                 str(Path(pdir) / ".." / "shared")],
            )

    def test_project_roots_default_asset_roots(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            _touch(root, "assets/x.pgg")
            proj = self._write_project(root, "{}", 1000)  # no asset_roots key
            calls: list[dict] = []
            layer = self._layer(root, calls)
            layer.delve.last_file = proj
            layer.load("assets/x.pgg")
            roots = calls[0]["args"]["lib_roots"]
            self.assertIn(str(Path(root) / "proj" / "assets"), roots)

    def test_broken_project_is_silently_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            _touch(root, "assets/x.pgg")
            proj = self._write_project(root, "{not json", 1000)
            calls: list[dict] = []
            layer = self._layer(root, calls)
            layer.delve.last_file = proj
            resp = layer.load("assets/x.pgg")
            self.assertTrue(resp["ok"])
            # dir(assets/x.pgg) == the delve assets root; deduped to one entry.
            roots = calls[0]["args"]["lib_roots"]
            self.assertEqual(roots, [str(Path(root) / "assets")])

    def test_project_roots_cached_by_mtime(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            _touch(root, "assets/x.pgg")
            proj = self._write_project(root, '{"asset_roots": ["assets"]}', 1000)
            calls: list[dict] = []
            layer = self._layer(root, calls)
            layer.delve.last_file = proj
            layer.load("assets/x.pgg")
            self.assertIn(str(Path(root) / "proj" / "assets"),
                          calls[-1]["args"]["lib_roots"])

            # Same mtime -> cached read (file content changed but not re-read).
            self._write_project(root, '{"asset_roots": ["shared"]}', 1000)
            layer.load("assets/x.pgg")
            self.assertIn(str(Path(root) / "proj" / "assets"),
                          calls[-1]["args"]["lib_roots"])
            self.assertNotIn(str(Path(root) / "proj" / "shared"),
                             calls[-1]["args"]["lib_roots"])

            # Bumped mtime -> re-read.
            self._write_project(root, '{"asset_roots": ["shared"]}', 2000)
            layer.load("assets/x.pgg")
            self.assertIn(str(Path(root) / "proj" / "shared"),
                          calls[-1]["args"]["lib_roots"])

    def test_render_probe_docs_forward_and_file_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            _touch(root, "assets/x.pgg")
            calls: list[dict] = []
            layer = self._layer(root, calls)
            layer.load("assets/x.pgg")
            loaded = calls[0]["args"]["path"]

            layer.render("facing", ortho="front", size=[640, 360])
            self.assertEqual(calls[-1]["op"], "render")
            self.assertEqual(calls[-1]["args"]["node"], "facing")
            self.assertEqual(calls[-1]["args"]["ortho"], "front")
            self.assertEqual(calls[-1]["args"]["size"], [640, 360])
            self.assertNotIn("target", calls[-1]["args"])  # None filtered out
            self.assertEqual(calls[-1]["args"]["file"], loaded)  # last_file fallback

            layer.probe(specs=["facing:stats", "facing:schema"])
            self.assertEqual(calls[-1]["op"], "probe")
            self.assertEqual(calls[-1]["args"]["specs"], ["facing:stats", "facing:schema"])

            layer.docs("elem_zone", file="/other.pgg")
            self.assertEqual(calls[-1]["op"], "docs")
            self.assertEqual(calls[-1]["args"]["file"], "/other.pgg")  # explicit wins

    def test_params_forward_and_file_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            _touch(root, "assets/x.pgg")
            calls: list[dict] = []
            layer = self._layer(root, calls)
            layer.load("assets/x.pgg")
            loaded = calls[0]["args"]["path"]

            layer.params({"seg": "@x.seg.points.json", "stories": 2})
            self.assertEqual(calls[-1]["op"], "params")
            self.assertEqual(calls[-1]["args"]["seg"], "@x.seg.points.json")
            self.assertEqual(calls[-1]["args"]["file"], loaded)  # last_file fallback

            layer.params({"stories": 3}, file="/other.pgg")
            self.assertEqual(calls[-1]["args"]["file"], "/other.pgg")  # explicit wins

    def test_render_out_absolutized_against_delve_root(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            _touch(root, "assets/x.pgg")
            calls: list[dict] = []
            layer = self._layer(root, calls)
            layer.load("assets/x.pgg")

            layer.render("facing", out="tmp/shots/a.png")
            self.assertEqual(calls[-1]["args"]["out"], str(Path(root) / "tmp" / "shots" / "a.png"))

            layer.render("facing", out="/abs/b.png")
            self.assertEqual(calls[-1]["args"]["out"], "/abs/b.png")  # absolute passes through

            layer.render("facing")
            self.assertNotIn("out", calls[-1]["args"])  # None keeps the server default


if __name__ == "__main__":
    unittest.main()
