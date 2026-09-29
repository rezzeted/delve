cmake_minimum_required(VERSION 3.20.0 FATAL_ERROR)
include(cmake/utils.cmake)

project(delve VERSION 0.1.0 LANGUAGES CXX C)
if(APPLE)
    # Standalone sokol-приложения компилируют main.cpp как Objective-C++
    # (nw_configure_sokol_app); Ninja/Makefile-генераторы требуют явного
    # включения языка (Xcode включает его сам). pgg_preview тоже помечает
    # PreviewCapture.cpp как OBJCXX.
    enable_language(OBJCXX)
endif()
print_header("delve")
check_preprocessor_configuration()
configure_common_param()
if(MSVC)
    add_compile_options(/MP)
endif()

#use grouping
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE)
    set(OpenGL_GL_PREFERENCE "GLVND" CACHE STRING "" FORCE)
    set(OPENGL_INCLUDE_DIR "/System/Library/Frameworks/OpenGL.framework/Headers" CACHE PATH "" FORCE)
    set(OPENGL_gl_LIBRARY "/System/Library/Frameworks/OpenGL.framework/OpenGL" CACHE FILEPATH "" FORCE)
elseif(UNIX)
    set(OpenGL_GL_PREFERENCE "GLVND" CACHE STRING "" FORCE)
endif()

find_package(spdlog CONFIG REQUIRED)

enable_testing()

set(_delve_submodule_markers
    thirdparty/pgg/src/libs/pgg/CMakeLists.txt
    thirdparty/pgg/src/libs/pgg_preview/CMakeLists.txt
    thirdparty/level-synth/src/libs/edgar/CMakeLists.txt
)
foreach(_rel IN LISTS _delve_submodule_markers)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${_rel}")
        message(FATAL_ERROR
            "Missing ${_rel}. Initialize submodules without nesting their own vcpkg:\n"
            "  git submodule update --init thirdparty/pgg thirdparty/level-synth toolchain/vcpkg")
    endif()
endforeach()

# Library roots only. The upstream projects are full applications; adding them
# whole would pull their apps, tests, and CMAKE_SOURCE_DIR paths.
add_subdirectory(thirdparty/pgg/src/libs/pgg)
# pgg registers pgg_parser_regen_diff against ${CMAKE_SOURCE_DIR}/tools/pgg,
# which is the pgg repo, not Delve. Tests are directory-scoped, so the property
# has to be set in that directory (CMake 3.28).
set_property(
    TEST pgg_parser_regen_diff
    DIRECTORY "${CMAKE_SOURCE_DIR}/thirdparty/pgg/src/libs/pgg"
    PROPERTY DISABLED TRUE
)
# In the pgg repo this path comes from src/libs/CMakeLists.txt include_directories.
# We add only the library, so consumers of <pgg/src/...> (pgg_preview) need it here.
target_include_directories(pgg PUBLIC "${CMAKE_SOURCE_DIR}/thirdparty/pgg/src/libs")
add_subdirectory(thirdparty/pgg/src/libs/pgg_preview)
add_subdirectory(thirdparty/level-synth/src/libs/edgar)

add_subdirectory(src/libs)
add_subdirectory(src/apps)
