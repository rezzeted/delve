# DelveViewer: empty Sokol/ImGui window. Sibling of the future CLI; does not
# link pgg or edgar.
find_package(imgui CONFIG REQUIRED)

nw_add_console_app(
    GROUP apps/tools
    LIBS
        spdlog::spdlog_header_only
        imgui::imgui
)

find_path(SOKOL_INCLUDE_DIRS "sokol_app.h")
if(NOT SOKOL_INCLUDE_DIRS)
    message(FATAL_ERROR "Sokol headers not found (sokol_app.h). Make sure vcpkg dependency 'sokol' is installed.")
endif()
target_include_directories(${AppName} PRIVATE ${SOKOL_INCLUDE_DIRS})

nw_configure_sokol_app(${AppName})
