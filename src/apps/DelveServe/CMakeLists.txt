# DelveServe: CPU RPC daemon of the delve pipeline (docs/mcp_v1.md) — project
# slots with the warm F8 unit cache. ServeRpcServer is an adapted copy of
# thirdparty/pgg/src/apps/PggServe (line-delimited JSON over TCP); the
# submodule is not linked from here, only the delve pipeline libs.
# nlohmann_json is found at src/libs/delve scope only; re-find it here so the
# imported target is visible (spdlog is already found at the repo root).
find_package(nlohmann_json CONFIG REQUIRED)

nw_add_console_app(
    GROUP apps/tools
    LIBS
        delve
        delve_layout
        delve_fill
        delve_check
        delve_export
        spdlog::spdlog_header_only
        nlohmann_json::nlohmann_json
)

# Smoke: a real server on 127.0.0.1:0 plus real TCP clients. The working
# directory is the repo root so the relative fixture paths and the ./assets
# fallback resolve like in the DelveCli smokes.
add_test(NAME DelveServe_smoke COMMAND DelveServe --smoke)
set_tests_properties(DelveServe_smoke PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Python wire-protocol smoke (tools/delve_mcp): a real DelveServe on a free
# port plus a stdlib raw-socket client — guards the Python<->C++ framing.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME DelveServe_rpc_py COMMAND ${Python3_EXECUTABLE}
           ${CMAKE_SOURCE_DIR}/tools/delve_mcp/tests/integration_smoke.py
           $<TARGET_FILE:DelveServe> ${CMAKE_SOURCE_DIR})
  set_tests_properties(DelveServe_rpc_py PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
