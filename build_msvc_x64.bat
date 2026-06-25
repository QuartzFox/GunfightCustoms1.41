@echo off
setlocal EnableExtensions
cd /d "%~dp0"

REM Build iw8_stringtable_sidecar.dll as x64 from a normal Command Prompt.
REM This auto-finds and calls vcvars64.bat, so you do not need the
REM "x64 Native Tools Command Prompt" shortcut.

set "VCVARS64="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%I\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64=%%I\VC\Auxiliary\Build\vcvars64.bat"
    )
)

if not defined VCVARS64 if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS64 if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS64 if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS64 if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if not defined VCVARS64 (
    echo [ERROR] Could not find vcvars64.bat.
    echo.
    echo Open Visual Studio Installer, choose Modify, and install:
    echo   - Desktop development with C++
    echo   - MSVC v143 x64/x86 build tools
    echo   - Windows 10 or 11 SDK
    echo.
    pause
    exit /b 1
)

echo Using:
echo   "%VCVARS64%"
echo.

call "%VCVARS64%"
if errorlevel 1 (
    echo.
    echo [ERROR] vcvars64.bat failed.
    pause
    exit /b 1
)

echo.
echo Building x64 DLL...
cl /nologo /std:c++17 /EHsc /O2 /MT /LD iw8_stringtable_sidecar.cpp /link /OUT:iw8_stringtable_sidecar.dll

if errorlevel 1 (
    echo.
    echo [ERROR] Build failed.
    pause
    exit /b 1
)

echo.
dumpbin /headers iw8_stringtable_sidecar.dll | findstr machine
echo.
echo Expected output should include:
echo   8664 machine ^(x64^)
echo.
echo Built iw8_stringtable_sidecar.dll
pause
endlocal
