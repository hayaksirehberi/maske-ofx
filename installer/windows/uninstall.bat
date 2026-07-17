@echo off
setlocal enableextensions
::
:: Maske — uninstaller (for the install.bat / no-toolchain install path).
:: Double-click to remove the plugin. Self-elevates.
::

set "BUNDLE=HistoryBrush.ofx.bundle"
set "DEST=%CommonProgramFiles%\OFX\Plugins"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator privileges...
    powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)

echo Removing "%DEST%\%BUNDLE%" ...

tasklist /FI "IMAGENAME eq Resolve.exe" 2>nul | find /I "Resolve.exe" >nul
if %errorlevel% equ 0 (
    echo WARNING: DaVinci Resolve is running. Quit it first, then run this again.
    echo.
    pause
    exit /b 1
)

if exist "%DEST%\%BUNDLE%" (
    rmdir /S /Q "%DEST%\%BUNDLE%"
    echo Done.
) else (
    echo Nothing to remove - plugin not found.
)
echo.
pause
endlocal
