@echo off
setlocal

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=vs2022"

if not exist toolchain\vcpkg\vcpkg.exe (
    echo Bootstrapping vcpkg...
    call toolchain\vcpkg\bootstrap-vcpkg.bat -disableMetrics
    if errorlevel 1 (
        echo vcpkg bootstrap failed.
        exit /b %errorlevel%
    )
)

cmake --preset "%PRESET%"
if errorlevel 1 (
    echo CMake generation failed.
    exit /b %errorlevel%
)

echo Visual Studio solution generated.
echo Solution: _intermediate_64\delve.sln
