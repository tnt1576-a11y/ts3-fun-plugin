@echo off
setlocal
set "ROOT=%~dp0"
set "PATH=%ROOT%tools\w64devkit\bin;%PATH%"
if not exist "%ROOT%tests\bin" mkdir "%ROOT%tests\bin"
windres "%ROOT%res\resource.rc" -I "%ROOT%res" -o "%ROOT%tests\bin\resource.o"
if errorlevel 1 exit /b 1
g++ -std=c++11 -m64 -O0 -g -static -Wall -Wextra -I "%ROOT%sdk\include" -I "%ROOT%res" "%ROOT%tests\copies_test.cpp" "%ROOT%tests\bin\resource.o" -o "%ROOT%tests\bin\copies_test.exe" -luser32 -lgdi32 -lcomdlg32 -ladvapi32 -lkernel32
if errorlevel 1 exit /b 1
"%ROOT%tests\bin\copies_test.exe"
if errorlevel 1 exit /b 1
if not exist "C:\Program Files\TeamSpeak 3 Client\Qt5Widgets.dll" exit /b 0
g++ -std=c++11 -m64 -O0 -g -static "%ROOT%tests\tab_ui_test.cpp" -o "%ROOT%tests\bin\tab_ui_test.exe" -luser32
if errorlevel 1 exit /b 1
"%ROOT%tests\bin\tab_ui_test.exe"
exit /b %errorlevel%
