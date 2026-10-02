@echo off
setlocal
REM ---------------------------------------------------------------------------
REM WuwaUID installer
REM
REM Copies WuwaUID.addon64 next to the game executable.
REM
REM Usage:
REM     install.bat                     probe the usual install locations
REM     install.bat "D:\path\Binaries\Win64"    use this directory
REM
REM The game must be closed: Windows locks a loaded DLL, so an add-on cannot be
REM replaced while the game is running.
REM ---------------------------------------------------------------------------

set "ADDON=%~dp0WuwaUID.addon64"
set "GAME_DIR=%~1"

if not exist "%ADDON%" (
	echo [!] WuwaUID.addon64 not found next to this script.
	pause
	exit /b 1
)

REM --- probe the common layouts (Steam / Epic / standalone, drives C-F) ------
if not defined GAME_DIR (
	for %%R in (C D E F G) do (
		for %%P in (
			"%%R:\SteamLibrary\steamapps\common\Wuthering Waves"
			"%%R:\Program Files (x86)\Steam\steamapps\common\Wuthering Waves"
			"%%R:\Program Files\Steam\steamapps\common\Wuthering Waves"
			"%%R:\Program Files (x86)\Epic Games\WutheringWavesj3oFh"
			"%%R:\Program Files\Epic Games\WutheringWavesj3oFh"
			"%%R:\Wuthering Waves"
			"%%R:\Game\Wuthering Waves"
			"%%R:\Games\Wuthering Waves"
		) do (
			if not defined GAME_DIR (
				for %%S in (
					"%%~P\Wuthering Waves Game\Client\Binaries\Win64"
					"%%~P\Client\Binaries\Win64"
					"%%~P\Binaries\Win64"
				) do (
					if not defined GAME_DIR if exist "%%~S\Client-Win64-Shipping.exe" set "GAME_DIR=%%~S"
				)
			)
		)
	)
)

if not defined GAME_DIR (
	echo [!] Could not find the game automatically.
	echo.
	echo     Run this script again with the directory that contains
	echo     Client-Win64-Shipping.exe, for example:
	echo.
	echo         install.bat "D:\Wuthering Waves\Wuthering Waves Game\Client\Binaries\Win64"
	echo.
	pause
	exit /b 1
)

if not exist "%GAME_DIR%\Client-Win64-Shipping.exe" (
	echo [!] Client-Win64-Shipping.exe not found in:
	echo     %GAME_DIR%
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
	echo [!] Copy failed. Check permissions on:
	echo     %GAME_DIR%
	pause
	exit /b 1
)

REM Drop the status file so the next launch reports fresh numbers.
del /Q "%GAME_DIR%\WuwaUID.status.txt" 2>nul

echo [OK] Installed to %GAME_DIR%
echo      Expect ReShade.log to show "Registered add-on "WuwaUID"" on next launch.
pause
