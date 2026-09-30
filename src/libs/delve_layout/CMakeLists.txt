# delve_layout: F2 catalog + F3 generation (edgar types live here, not in delve).
add_library(delve_layout STATIC
    pch.h
    catalog.h
    catalog.cpp
    generate.h
    generate.cpp
)
target_include_directories(delve_layout PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_precompile_headers(delve_layout PRIVATE pch.h)
target_link_libraries(delve_layout PUBLIC delve edgar)
target_compile_features(delve_layout PUBLIC cxx_std_20)
set_property(TARGET delve_layout PROPERTY FOLDER libs)
