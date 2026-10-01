add_library(delve_fill STATIC
    fill_ra3.cpp
    fill.cpp
    cache.cpp
    assets.cpp
)

target_include_directories(delve_fill PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(delve_fill PUBLIC delve pgg)
target_compile_features(delve_fill PUBLIC cxx_std_20)
