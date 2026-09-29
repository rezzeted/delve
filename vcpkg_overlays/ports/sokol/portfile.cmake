# header-only library (overlay port)
#
# Overlay rationale:
# - keep Sokol current enough for the Dear ImGui version installed by vcpkg
# - upstream Sokol carries util/sokol_imgui.h compatibility fixes that are
#   preferable to maintaining local patches for ImGui internals.
#
# Patches:
# - macos-framebuffer-readback.patch: CAMetalLayer.framebufferOnly = false on
#   macOS so a Sokol app can blit-read the drawable texture (used by PGG
#   PggViewer; kept here because the overlay is shared).

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO floooh/sokol
    REF 082152c1f36fda5fe5aa3de8de47af55c17101b9
    SHA512 ce0a94ce27baa19254b63a8281462bf278b0556f51ed297141fa115b715ce936da108d616ca643ddc9f5e21601fdeabe59c938e3e49ed75adf25f5e622abfbef
    HEAD_REF master
    PATCHES
        macos-framebuffer-readback.patch
)

file(GLOB SOKOL_INCLUDE_FILES "${SOURCE_PATH}/*.h")
file(COPY ${SOKOL_INCLUDE_FILES} DESTINATION "${CURRENT_PACKAGES_DIR}/include")

file(GLOB SOKOL_UTIL_INCLUDE_FILES "${SOURCE_PATH}/util/*.h")
file(COPY ${SOKOL_UTIL_INCLUDE_FILES} DESTINATION "${CURRENT_PACKAGES_DIR}/include/util")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

