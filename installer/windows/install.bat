@echo off
setlocal enableextensions
::
:: Maske — no-toolchain Windows installer.
:: Ship this file inside a ZIP next to HistoryBrush.ofx.bundle. The user just
:: extracts and double-clicks install.bat — it elevates itself and copies the
:: plugin into the standard OFX folder. No Inno Setup, no manual folder work.
::

set "BUNDLE=HistoryBrush.ofx.bundle"
set "DEST=%CommonProgramFiles%\OFX\Plugins"

:: --- Re-launch elevated if we are not already admin ---
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator privileges...
    powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)

echo ============================================
echo   Maske OFX plugin installer
echo ============================================
echo.

:: --- Locate the bundle next to this script ---
if not exist "%~dp0%BUNDLE%" (
    echo ERROR: "%BUNDLE%" was not found next to this installer.
    echo Keep install.bat and the %BUNDLE% folder in the same place.
    echo.
    pause
    exit /b 1
)

:: --- Warn if Resolve is running (locked .ofx cannot be replaced) ---
tasklist /FI "IMAGENAME eq Resolve.exe" 2>nul | find /I "Resolve.exe" >nul
if %errorlevel% equ 0 (
    echo WARNING: DaVinci Resolve is running. Please quit it first,
    echo          otherwise the plugin file may be locked.
    echo.
    choice /M "Continue anyway"
    if errorlevel 2 exit /b 1
)

echo Installing to:
echo   "%DEST%\%BUNDLE%"
echo.

if not exist "%DEST%" mkdir "%DEST%"

:: Clean any previous copy so stale files don't linger, then copy fresh.
if exist "%DEST%\%BUNDLE%" rmdir /S /Q "%DEST%\%BUNDLE%"
xcopy "%~dp0%BUNDLE%" "%DEST%\%BUNDLE%\" /E /I /Y >nul

if %errorlevel% equ 0 (
    echo Done.
    echo.
    echo Launch DaVinci Resolve Studio, open the Color page, and find the plugin
    echo under OpenFX ^> Mustafa Ekinci ^> Maske.
) else (
    echo ERROR: copy failed with code %errorlevel%.
)
echo.
pause
endlocal
