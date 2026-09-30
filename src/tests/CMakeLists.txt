find_package(GTest REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(delve_d0_test delve_d0_test.cpp)
target_link_libraries(delve_d0_test PRIVATE GTest::gtest_main delve_d0 pgg nlohmann_json::nlohmann_json)
target_compile_definitions(delve_d0_test PRIVATE DELVE_D0_DIR="${CMAKE_SOURCE_DIR}/docs/d0")
target_compile_features(delve_d0_test PRIVATE cxx_std_20)
set_property(TARGET delve_d0_test PROPERTY FOLDER tests)

add_test(NAME delve_d0_test COMMAND delve_d0_test)

add_executable(delve_project_test delve_project_test.cpp)
target_link_libraries(delve_project_test PRIVATE GTest::gtest_main delve)
target_compile_definitions(delve_project_test PRIVATE
    DELVE_TEST_DATA="${CMAKE_SOURCE_DIR}/src/tests/data")
target_compile_features(delve_project_test PRIVATE cxx_std_20)
set_property(TARGET delve_project_test PROPERTY FOLDER tests)

add_test(NAME delve_project_test COMMAND delve_project_test)

add_executable(delve_ir_test delve_ir_test.cpp)
target_link_libraries(delve_ir_test PRIVATE GTest::gtest_main delve nlohmann_json::nlohmann_json)
target_compile_definitions(delve_ir_test PRIVATE
    DELVE_TEST_DATA="${CMAKE_SOURCE_DIR}/src/tests/data"
    DELVE_D0_DIR="${CMAKE_SOURCE_DIR}/docs/d0")
target_compile_features(delve_ir_test PRIVATE cxx_std_20)
set_property(TARGET delve_ir_test PROPERTY FOLDER tests)

add_test(NAME delve_ir_test COMMAND delve_ir_test)

add_executable(delve_assets_test delve_assets_test.cpp)
target_link_libraries(delve_assets_test PRIVATE GTest::gtest_main delve pgg nlohmann_json::nlohmann_json)
target_compile_definitions(delve_assets_test PRIVATE DELVE_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
target_compile_features(delve_assets_test PRIVATE cxx_std_20)
set_property(TARGET delve_assets_test PROPERTY FOLDER tests)

add_test(NAME delve_assets_test COMMAND delve_assets_test)

add_executable(delve_fill_test delve_fill_test.cpp)
target_link_libraries(delve_fill_test PRIVATE GTest::gtest_main delve_fill nlohmann_json::nlohmann_json)
target_compile_definitions(delve_fill_test PRIVATE
    DELVE_TEST_DATA="${CMAKE_SOURCE_DIR}/src/tests/data"
    DELVE_D0_DIR="${CMAKE_SOURCE_DIR}/docs/d0"
    DELVE_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
target_compile_features(delve_fill_test PRIVATE cxx_std_20)
set_property(TARGET delve_fill_test PROPERTY FOLDER tests)

add_test(NAME delve_fill_test COMMAND delve_fill_test)

add_executable(delve_check_test delve_check_test.cpp)
target_link_libraries(delve_check_test PRIVATE GTest::gtest_main delve_check)
target_compile_definitions(delve_check_test PRIVATE
    DELVE_TEST_DATA="${CMAKE_SOURCE_DIR}/src/tests/data"
    DELVE_D0_DIR="${CMAKE_SOURCE_DIR}/docs/d0"
    DELVE_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
target_compile_features(delve_check_test PRIVATE cxx_std_20)
set_property(TARGET delve_check_test PROPERTY FOLDER tests)

add_test(NAME delve_check_test COMMAND delve_check_test)
