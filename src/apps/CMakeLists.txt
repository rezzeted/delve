file(GLOB_RECURSE SUB_CMAKELISTS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "*/CMakeLists.txt")

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

foreach(sub_cmake ${SUB_CMAKELISTS})
    get_filename_component(SUB_DIR ${sub_cmake} DIRECTORY)
    print_info("Add app: ${SUB_DIR}")
    add_subdirectory(${SUB_DIR})
endforeach()
