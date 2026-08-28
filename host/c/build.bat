@echo off
REM Build the C client. Run from this directory; libusb is vendored alongside,
REM so CMake needs no -DLIBUSB_ROOT.
@REM cd /d "%~dp0"
echo.

set in_cmd=%1


if [%1]==[] (
    if exist "./build/main.exe" (
        echo Deleting old executable
        del ".\build\main.exe" /F /Q
        @REM rmdir /s /q build
    )
) else (
    if "%in_cmd%"=="run" (
        goto :run
    )
    if "%in_cmd%"=="clear" (
        goto :compile
    )
    if "%in_cmd%"=="clean" (
        goto :setup
    )
    if "%in_cmd%"=="/?" or "%in_cmd%"=="help" or "%in_cmd%"=="h" (
        echo Use "run" to only run without recompiling
        echo Use "clear" to clear console before execution
        echo Use "clean" to setup CMake and clear last build
        goto:eof
    )
)

:setup
    rmdir /s /q build
    cmake -B build -G "MinGW Makefiles" -DCMAKE_C_COMPILER=C:/ProgramData/mingw64/mingw64/bin/gcc.exe -DCMAKE_TOOLCHAIN_FILE=%USERPROFILE%/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-mingw-dynamic
    
    cmake -S . -B build -G "MinGW Makefiles" || exit /b 1
goto:eof

:compile
echo Compiling main.exe...
cmake --build build || exit /b 1
echo Compiling done !
echo:
echo:
echo:
if "%in_cmd%"=="run" or "%in_cmd%"=="clear"  (
    goto :run
)
goto:eof

:run
if exist "./build/main.exe" (    
    color 07
    "./build/main.exe"
) else (
    echo Failed to compile !
)
goto:eof
@REM pause
