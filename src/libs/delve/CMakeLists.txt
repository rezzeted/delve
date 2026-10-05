find_package(nlohmann_json CONFIG REQUIRED)

add_library(delve STATIC
    project.cpp
    layout.cpp
    ir.cpp
    topo.cpp
)

target_include_directories(delve PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_precompile_headers(delve PRIVATE pch.h)
target_link_libraries(delve PUBLIC nlohmann_json::nlohmann_json)
