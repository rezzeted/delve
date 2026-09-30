add_library(delve_check STATIC
    check.cpp
)

target_include_directories(delve_check PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(delve_check PUBLIC delve delve_fill pgg)
target_compile_features(delve_check PUBLIC cxx_std_20)
