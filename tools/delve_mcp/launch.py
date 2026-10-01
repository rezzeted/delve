"""Bootstrap the Delve MCP venv (stdlib only) and exec the stdio server.

Cursor (and OpenCode) start this module as the single MCP server ``delve``:

    python3 -m tools.delve_mcp.launch

Interpreter discovery, venv, and ``pip install`` live here so every OS shares
one bootstrap. Adapted copy of thirdparty/pgg/tools/pgg_mcp/launch.py (same
bootstrap pattern; the submodule is not modified).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional


def _checkout_root() -> Path:
    return Path(__file__).resolve().parent.parent.parent


def _repo_root() -> Path:
    """Repo root from ``DELVE_REPO_ROOT``, else the directory that contains this file.

    Cursor may substitute ``${workspaceFolder}`` as a literal ``~/...`` in the
    env while the process cwd is already absolute. ``Path.resolve()`` does not
    expand ``~`` and would join it onto cwd. A missing directory must not abort
    startup: the module path is always inside the checkout.
    """
    fallback = _checkout_root()
    env = os.environ.get("DELVE_REPO_ROOT")
    if not env:
        return fallback
    root = Path(env).expanduser()
    if not root.is_absolute():
        root = Path.cwd() / root
    try:
        root = root.resolve()
    except OSError:
        return fallback
    if root.is_dir():
        return root
    print(
        f"tools.delve_mcp.launch: DELVE_REPO_ROOT={env!r} is not a directory, using {fallback}",
        file=sys.stderr,
    )
    return fallback


def _python_is_310(exe: str) -> bool:
    if not exe:
        return False
    try:
        proc = subprocess.run(
            [exe, "-c", "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except OSError:
        return False
    return proc.returncode == 0


def _windows_py_launcher() -> Optional[str]:
    try:
        proc = subprocess.run(
            ["py", "-3", "-c", "import sys; print(sys.executable)"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
    except OSError:
        return None
    if proc.returncode != 0:
        return None
    path = (proc.stdout or "").strip()
    return path or None


def pick_bootstrap_python() -> str:
    """Python ≥ 3.10 to create/use the venv. Prefer the current interpreter."""
    candidates: list[str] = []
    if sys.executable:
        candidates.append(sys.executable)
    for name in ("python3", "python"):
        found = shutil.which(name)
        if found:
            candidates.append(found)
    for path in (
        "/opt/homebrew/bin/python3",
        "/usr/local/bin/python3",
        "/usr/bin/python3",
    ):
        candidates.append(path)
    py_exe = _windows_py_launcher()
    if py_exe:
        candidates.append(py_exe)

    seen: set[str] = set()
    for exe in candidates:
        try:
            resolved = str(Path(exe).resolve())
        except OSError:
            continue
        if resolved in seen:
            continue
        seen.add(resolved)
        if os.path.isfile(resolved) and _python_is_310(resolved):
            return resolved
    raise SystemExit(
        "tools.delve_mcp.launch: no suitable python found (need python3 >= 3.10)"
    )


def _venv_python(venv: Path) -> Path:
    if os.name == "nt":
        return venv / "Scripts" / "python.exe"
    return venv / "bin" / "python"


def _mcp_importable(py: Path) -> bool:
    proc = subprocess.run(
        [str(py), "-c", "import mcp"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return proc.returncode == 0


def ensure_venv(root: Path, bootstrap_py: str) -> Path:
    venv = root / "tools" / "delve_mcp" / ".venv"
    py = _venv_python(venv)
    req = root / "tools" / "delve_mcp" / "requirements.txt"
    if not py.is_file():
        print(f"tools.delve_mcp.launch: creating venv {venv} with {bootstrap_py}",
              file=sys.stderr)
        subprocess.check_call([bootstrap_py, "-m", "venv", str(venv)])
    if not _mcp_importable(py):
        subprocess.check_call(
            [str(py), "-m", "pip", "install", "-q", "-r", str(req)]
        )
    return py


def main() -> None:
    root = _repo_root()
    os.chdir(root)
    os.environ["DELVE_REPO_ROOT"] = str(root)
    os.environ.setdefault("PYTHONUTF8", "1")
    os.environ.setdefault("PYTHONIOENCODING", "utf-8")
    existing = os.environ.get("PYTHONPATH", "")
    os.environ["PYTHONPATH"] = (
        str(root) if not existing else str(root) + os.pathsep + existing
    )

    # This file must parse on macOS system Python 3.9: we re-exec 3.10+ here.
    if sys.version_info < (3, 10):
        bootstrap = pick_bootstrap_python()
        if os.path.realpath(bootstrap) == os.path.realpath(sys.executable or ""):
            raise SystemExit(
                "tools.delve_mcp.launch: no suitable python found (need python3 >= 3.10)"
            )
        os.execv(bootstrap, [bootstrap, "-m", "tools.delve_mcp.launch"] + sys.argv[1:])

    bootstrap = pick_bootstrap_python()
    py = ensure_venv(root, bootstrap)
    os.execv(str(py), [str(py), "-m", "tools.delve_mcp"] + sys.argv[1:])


if __name__ == "__main__":
    main()
