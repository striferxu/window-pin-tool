@echo off
rem Build 32-bit (x86) version -- run in cmd / PowerShell, NOT Git Bash
rem Targets Windows 7 SP1 and later (_WIN32_WINNT=0x0601, subsystem 6.01)
rem Output name carries the version from VERSION.txt; old versions are kept.
setlocal

set VC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207
set SDK=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0

set INCLUDE=%VC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared
set LIB=%VC%\lib\x86;%SDK%\Lib\%SDKVER%\ucrt\x86;%SDK%\Lib\%SDKVER%\um\x86

cd /d "%~dp0"

rem ---- version (single source: VERSION.txt) ----
for /f "usebackq delims=" %%v in ("VERSION.txt") do set VER=%%v
if "%VER%"=="" (
    echo VERSION.txt missing or empty
    exit /b 1
)
echo [0/4] version %VER%

echo [1/4] generate version.h
python gen_version.py x86
if errorlevel 1 (
    echo VERSION HEADER FAILED
    exit /b 1
)

echo [2/4] resources (icon + version info, x86)
"%SDK%\bin\%SDKVER%\x86\rc.exe" /nologo /fo windowpin32.res windowpin.rc
if errorlevel 1 (
    echo RC FAILED
    exit /b 1
)

echo [3/4] compile x86 release build
"%VC%\bin\Hostx64\x86\cl.exe" /nologo /utf-8 /O1 /MT /EHsc /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /Fe:WindowPinLite-v%VER%-x86.exe windowpin.cpp windowpin32.res /link /SUBSYSTEM:WINDOWS,6.01 /OPT:REF /OPT:ICF gdiplus.lib
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
del /q *.obj 2>nul
echo [4/4] BUILD OK -^> WindowPinLite-v%VER%-x86.exe
