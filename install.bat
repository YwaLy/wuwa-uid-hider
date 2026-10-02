@echo off
setlocal
REM ---------------------------------------------------------------------------
REM WuwaUID installer
REM
REM Copies WuwaUID.addon64 next to the game executable. Edit GAME_DIR below if
REM your installation lives somewhere else (Steam and Epic use the same layout).
REM
REM The game must be closed: Windows locks a loaded DLL, so an add-on cannot be
REM replaced while the game is running.
REM ---------------------------------------------------------------------------

set "GAME_DIR=E:\Game\Wuthering Waves Game\Client\Binaries\Win64"
set "ADDON=%~dp0WuwaUID.addon64"

if not exist "%ADDON%" (
	echo [!] WuwaUID.addon64 not found next to this script.
	pause
	exit /b 1
)

if not exist "%GAME_DIR%" (
	echo [!] Game directory not found:
	echo     %GAME_DIR%
	echo     Edit GAME_DIR in this script and try again.
	pause
	exit /b 1
)

tasklist /FI "IMAGENAME eq Client-Win64-Shipping.exe" 2>nul | find /I "Client-Win64-Shipping.exe" >nul
if not errorlevel 1 (
	echo [!] Wuthering Waves is running. Close it first, then run this again.
	pause
	exit /b 1
)

copy /Y "%ADDON%" "%GAME_DIR%\WuwaUID.addon64" >nul
if errorlevel 1 (
	echo [!] Copy failed. Check GAME_DIR and permissions.
	pause
	exit /b 1
)

REM Drop the status file so the next launch reports fresh numbers.
del /Q "%GAME_DIR%\WuwaUID.status.txt" 2>nul

echo [OK] Installed to %GAME_DIR%
echo      Expect ReShade.log to show "Registered add-on "WuwaUID"" on next launch.
pause
