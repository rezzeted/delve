INTERMEDIATE_PATH="_intermediate_64"  

cmake -S . -B "$INTERMEDIATE_PATH" \
      -DCMAKE_TOOLCHAIN_FILE=toolchain/vcpkg/scripts/buildsystems/vcpkg.cmake \
      -DVCPKG_OVERLAY_PORTS="$PWD/vcpkg_overlays/ports" \
      -DVCPKG_INSTALL_OPTIONS="--x-buildtrees-root=d:/_build" \
      -G "Visual Studio 17 2022" \
      -A x64