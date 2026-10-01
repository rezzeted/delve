# Negative-path smoke (F10): an unknown project key must fail with exit 1 and a
# D101 diagnostic — the machine loop reads the code, not the prose.
execute_process(
    COMMAND "${CLI_EXE}" validate "${BAD_PROJECT}"
    RESULT_VARIABLE res
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)
if(NOT res EQUAL 1)
    message(FATAL_ERROR "expected exit 1, got ${res}\nstdout: ${out}\nstderr: ${err}")
endif()
if(NOT err MATCHES "D101")
    message(FATAL_ERROR "expected a D101 diagnostic, got:\n${err}")
endif()
message(STATUS "DelveCli_smoke_errors: exit 1 + D101 as expected")
