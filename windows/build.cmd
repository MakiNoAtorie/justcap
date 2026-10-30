@echo off
setlocal
cd /d %~dp0
if not exist build mkdir build
for /f %%n in ('git rev-list --count HEAD 2^>nul') do set BUILD=%%n
if not defined BUILD set BUILD=1
set DEST=%LOCALAPPDATA%\Programs\justcap
cl /nologo /std:c++17 /O2 /W4 /EHsc /DUNICODE /D_UNICODE /DBUILD_NUMBER=%BUILD% ^
   src\main.cpp src\overlay.cpp /Fo:build\ /Fe:build\justcap.exe ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib uuid.lib d2d1.lib dwrite.lib ^
   windowscodecs.lib winmm.lib shcore.lib advapi32.lib imm32.lib || exit /b 1
build\justcap.exe --selftest || (echo selftest FAILED & exit /b 1)
if "%1"=="install" (
    taskkill /f /im justcap.exe >nul 2>&1
    if not exist "%DEST%" mkdir "%DEST%"
    copy /y build\justcap.exe "%DEST%\" >nul
    start "" "%DEST%\justcap.exe"
)
