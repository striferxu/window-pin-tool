@echo off
rem WindowPinLite build script -- run in cmd / PowerShell, NOT Git Bash
setlocal

set VC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207
set SDK=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0

set INCLUDE=%VC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared
set LIB=%VC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64

cd /d "%~dp0"

echo [1/3] resources (icon + version info)
"%SDK%\bin\%SDKVER%\x64\rc.exe" /nologo /fo windowpin.res windowpin.rc
if errorlevel 1 (
    echo RC FAILED
    exit /b 1
)

if /i "%1"=="debug" (
    echo [2/3] compile debug build with logging
    "%VC%\bin\Hostx64\x64\cl.exe" /nologo /utf-8 /O1 /MT /EHsc /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /DWP_DEBUG_LOG /Fe:WindowPinDbg.exe windowpin.cpp windowpin.res /link /SUBSYSTEM:WINDOWS,6.01 gdiplus.lib
) else (
    echo [2/3] compile release build
    "%VC%\bin\Hostx64\x64\cl.exe" /nologo /utf-8 /O1 /MT /EHsc /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /Fe:WindowPinLite.exe windowpin.cpp windowpin.res /link /SUBSYSTEM:WINDOWS,6.01 /OPT:REF /OPT:ICF gdiplus.lib
)
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
del /q *.obj 2>nul
echo [3/3] BUILD OK
