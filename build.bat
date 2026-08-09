@echo off
REM ================================================================
REM  Build funplugin_win64.dll with the bundled w64devkit toolchain
REM ================================================================
setlocal

set ROOT=%~dp0
set TOOLS=%ROOT%tools\w64devkit\bin
set SDK=%ROOT%sdk\include
set OUT=funplugin_win64.dll

if not exist "%TOOLS%\g++.exe" (
    echo [ERROR] Toolchain not found at %TOOLS%
    echo         Unpack w64devkit into tools\w64devkit first.
    exit /b 1
)
if not exist "%SDK%\plugin.h" (
    echo [ERROR] TS3 Plugin SDK headers not found in %SDK%
    echo         Copy the SDK include\ files there first.
    exit /b 1
)

set PATH=%TOOLS%;%PATH%

echo [1/2] Compiling dialog resource...
windres "%ROOT%res\resource.rc" -I "%ROOT%res" -o "%ROOT%resource.o"
if errorlevel 1 exit /b 1

echo [2/2] Compiling and linking plugin...
g++ -shared -m64 -O2 -s ^
    -static -static-libgcc -static-libstdc++ ^
    -I "%SDK%" -I "%ROOT%res" ^
    "%ROOT%src\plugin.cpp" "%ROOT%resource.o" ^
    -o "%ROOT%%OUT%" ^
    -luser32 -lgdi32 -lcomdlg32 -ladvapi32 -lkernel32
if errorlevel 1 exit /b 1

echo.
echo Built %OUT%
exit /b 0
