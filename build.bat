@echo off
rem Build script for Simulated Typing (MSVC / x64).
rem Usage: build.bat
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [error] vswhere.exe not found. Install Visual Studio 2017 or later with the C++ workload.
    exit /b 1
)

set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo [error] No MSVC installation found. Install the "Desktop development with C++" workload.
    exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\VCvarsall.bat" x64 >nul
if errorlevel 1 (
    echo [error] Failed to initialize the MSVC environment.
    exit /b 1
)

if not exist build mkdir build

rem --- unit tests for the key-expansion logic (run first, fail fast) ---
cl /nologo /W4 /WX /utf-8 /EHsc /DUNICODE /D_UNICODE /Isrc /c tests\test_expand.cpp src\keysim.cpp /Fobuild\
if errorlevel 1 exit /b 1
link /nologo /OUT:build\test_expand.exe build\test_expand.obj build\keysim.obj
if errorlevel 1 exit /b 1
build\test_expand.exe
if errorlevel 1 (
    echo [error] Unit tests failed.
    exit /b 1
)

rem --- resources ---
rc /nologo /fo build\app.res /i res /i src res\app.rc
if errorlevel 1 exit /b 1

rem --- application ---
cl /nologo /W4 /WX /utf-8 /O2 /EHsc /DUNICODE /D_UNICODE /Isrc /c src\main.cpp src\hook.cpp src\typer.cpp src\keysim.cpp /Fobuild\
if errorlevel 1 exit /b 1

link /nologo /SUBSYSTEM:WINDOWS /MANIFEST:NO /OUT:SimulatedTyping.exe build\main.obj build\hook.obj build\typer.obj build\keysim.obj build\app.res user32.lib shell32.lib advapi32.lib
if errorlevel 1 exit /b 1

echo Build OK: SimulatedTyping.exe
endlocal
