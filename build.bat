@echo off
rem Build script for Simulated Typing (MSVC / x64).
rem Keep this file ASCII-only: cmd parses it under the system code page and
rem UTF-8 comments break line parsing.
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

call "%VSDIR%\VC\Auxiliary\Build\VCvarsall.bat" x64
if not "%errorlevel%"=="0" (
    echo [error] Failed to initialize the MSVC environment.
    exit /b 1
)

if not exist build mkdir build
if not exist build\test mkdir build\test

rem --- unit tests for the key-expansion logic (run first, fail fast) ---
rem NOTE: gates use "if not errorlevel==0" because "if errorlevel 1" is a
rem signed >=1 comparison and would miss crash-style negative exit codes.
rem /MT statically links the CRT so the built exe has no VC runtime DLL
rem dependency (explicit, do not rely on toolchain defaults).
cl /nologo /W4 /WX /utf-8 /O2 /MT /EHsc /DUNICODE /D_UNICODE /Isrc /c tests\test_expand.cpp src\keysim.cpp /Fobuild\test\
if not "%errorlevel%"=="0" exit /b 1
link /nologo /OUT:build\test\test_expand.exe build\test\test_expand.obj build\test\keysim.obj
if not "%errorlevel%"=="0" exit /b 1
build\test\test_expand.exe
if not "%errorlevel%"=="0" (
    echo [error] Unit tests failed.
    exit /b 1
)

rem --- resources ---
rem res\app.rc references "..\logo.ico", "app.manifest" and "resource.h":
rem data files (ico/manifest) resolve relative to the .rc file's own
rem directory; "resource.h" resolves through the /i src search path.
rc /nologo /fo build\app.res /i res /i src res\app.rc
if not "%errorlevel%"=="0" exit /b 1

rem --- application ---
rem Remove stale .obj files so a failed partial compile cannot link old objects.
del /q build\*.obj 2>nul
cl /nologo /W4 /WX /utf-8 /O2 /MT /EHsc /DUNICODE /D_UNICODE /Isrc /c src\main.cpp src\hook.cpp src\typer.cpp src\keysim.cpp /Fobuild\
if not "%errorlevel%"=="0" exit /b 1

link /nologo /SUBSYSTEM:WINDOWS /MANIFEST:NO /OUT:SimulatedTyping.exe build\main.obj build\hook.obj build\typer.obj build\keysim.obj build\app.res user32.lib shell32.lib advapi32.lib
if not "%errorlevel%"=="0" exit /b 1

echo Build OK: SimulatedTyping.exe
endlocal
