@echo off
rem Usage: build_vs2019.bat [Configuration] [Architecture]
rem Example: build_vs2019.bat Debug x64

taskkill /f /im MemoryMoniter.exe 2>nul || echo "!!! MemoryMoniter.exe is over !!!"

setlocal enabledelayedexpansion
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug
set ARCH=%2
if "%ARCH%"=="" set ARCH=x64

echo Using Configuration=%CONFIG% Architecture=%ARCH%

where cmake >nul 2>&1
if errorlevel 1 (
    echo Error: cmake not found in PATH. Please install CMake and ensure it's in PATH.
    exit /b 1
)

set BUILD_DIR=build
echo Configuring with CMake (Visual Studio 2019 generator)...
cmake -S . -B %BUILD_DIR% -G "Visual Studio 16 2019" -A %ARCH% -DCMAKE_BUILD_TYPE=%CONFIG%
if errorlevel 1 (
    echo CMake configuration failed.
    exit /b 2
)

echo Building...
cmake --build %BUILD_DIR% --config %CONFIG%
if errorlevel 1 (
    echo Build failed.
    exit /b 3
)

echo Build completed successfully. Output folder: %BUILD_DIR%\%CONFIG%
endlocal
exit /b 0
