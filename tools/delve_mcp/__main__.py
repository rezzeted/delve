"""``python -m tools.delve_mcp`` — stdio MCP server (venv already prepared).

Bootstrap (venv + deps) is ``python -m tools.delve_mcp.launch``.
"""

from tools.delve_mcp.server import mcp


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
