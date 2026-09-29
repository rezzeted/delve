# delve_d0: D0 scaffold (frozen-IR dump). Replaced by the F1-F4 implementation at D1/D2.
find_package(nlohmann_json CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)

add_library(delve_d0 STATIC
    pch.h
    preset_loader.hpp
    preset_loader.cpp
    ir_dump.h
    ir_dump.cpp
)
target_include_directories(delve_d0 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_precompile_headers(delve_d0 PRIVATE pch.h)
target_link_libraries(delve_d0 PUBLIC edgar nlohmann_json::nlohmann_json yaml-cpp::yaml-cpp)
target_compile_features(delve_d0 PUBLIC cxx_std_20)
set_property(TARGET delve_d0 PROPERTY FOLDER libs)
