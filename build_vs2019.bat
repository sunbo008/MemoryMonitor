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

rem Locate built executable and run it immediately
set EXE_NAME=MemoryMoniter.exe
set EXE_PATH=%BUILD_DIR%\%CONFIG%\%EXE_NAME%
if not exist "%EXE_PATH%" (
    set EXE_PATH=%BUILD_DIR%\%ARCH%\%CONFIG%\%EXE_NAME%
)

if not exist "%EXE_PATH%" (
    echo Warning: Executable not found: "%BUILD_DIR%\%CONFIG%\%EXE_NAME%" or "%BUILD_DIR%\%ARCH%\%CONFIG%\%EXE_NAME%"
    endlocal
    exit /b 4
)

echo Launching %EXE_PATH% ...
start "" "%EXE_PATH%"
endlocal
exit /b 0
