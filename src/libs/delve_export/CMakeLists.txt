add_library(delve_export STATIC
    export.cpp
)

target_include_directories(delve_export PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(delve_export PUBLIC delve_fill)
target_compile_features(delve_export PUBLIC cxx_std_20)
