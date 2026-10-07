@echo off
rem PS1 VJ - start the emulator(s) and the mixer in one go.
rem
rem   start-vj.bat GAME_A.cue               one game on channel A
rem   start-vj.bat GAME_A.cue GAME_B.cue    two games, A and B
rem
rem Or drag one or two .cue files onto this file.
rem The first run downloads the pcsx-redux VJ fork from GitHub into
rem pcsx-redux-A next to this script (and copies it to pcsx-redux-B for a
rem second game: two emulators need two folders). Needs internet once.

setlocal
cd /d "%~dp0"

set FORK_TAG=v0.7.11
set FORK_URL=https://github.com/pbfmp5g55-gif/pcsx-redux/releases/download/%FORK_TAG%/pcsx-redux-vj-windows-x64-%FORK_TAG%.zip
set EMU_A=%~dp0pcsx-redux-A
set EMU_B=%~dp0pcsx-redux-B
set BIOS=%~dp0openbios.bin
set MIXER=%~dp0vj-mix-spike1.exe

if "%~1"=="" goto usage
if not exist "%~1" goto nogameA
if "%~2"=="" goto gamesok
if not exist "%~2" goto nogameB
:gamesok
if not exist "%BIOS%" goto nobios
if not exist "%MIXER%" goto nomixer

if exist "%EMU_A%\pcsx-redux.exe" goto haveA
echo.
echo   Downloading the emulator (pcsx-redux VJ fork %FORK_TAG%), about 40 MB ...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; $z=Join-Path $env:TEMP 'psvj-fork.zip'; Invoke-WebRequest -UseBasicParsing '%FORK_URL%' -OutFile $z; Expand-Archive -Force $z '%EMU_A%'; Remove-Item $z"
if errorlevel 1 goto dlfail
if not exist "%EMU_A%\pcsx-redux.exe" goto dlfail
:haveA

if "%~2"=="" goto runA
if exist "%EMU_B%\pcsx-redux.exe" goto runAB
echo   Making a second emulator folder for channel B ...
xcopy /e /i /q /y "%EMU_A%" "%EMU_B%" >nul
if errorlevel 1 goto copyfail

:runAB
start "" /d "%EMU_A%" "%EMU_A%\pcsx-redux.exe" -bios "%BIOS%" -iso "%~f1" -vjring Local\vj-mix-prim-A -run
start "" /d "%EMU_B%" "%EMU_B%\pcsx-redux.exe" -bios "%BIOS%" -iso "%~f2" -vjring Local\vj-mix-prim-B -run
timeout /t 5 /nobreak >nul
start "" /d "%~dp0" "%MIXER%" --attach-a Local\vj-mix-prim-A --attach-b Local\vj-mix-prim-B
goto end

:runA
start "" /d "%EMU_A%" "%EMU_A%\pcsx-redux.exe" -bios "%BIOS%" -iso "%~f1" -vjring Local\vj-mix-prim-A -run
timeout /t 5 /nobreak >nul
start "" /d "%~dp0" "%MIXER%" --attach-a Local\vj-mix-prim-A
goto end

:usage
echo.
echo   Usage: start-vj.bat GAME_A.cue [GAME_B.cue]
echo   (or drag one or two .cue files onto start-vj.bat)
echo.
pause
exit /b 1

:nogameA
echo.
echo   Game A not found: %~1
echo.
pause
exit /b 1

:nogameB
echo.
echo   Game B not found: %~2
echo.
pause
exit /b 1

:nobios
echo.
echo   openbios.bin is missing next to this script.
echo.
pause
exit /b 1

:nomixer
echo.
echo   vj-mix-spike1.exe is missing next to this script.
echo.
pause
exit /b 1

:dlfail
echo.
echo   Could not download or unpack the emulator from
echo   %FORK_URL%
echo   Download that zip by hand and unpack it into
echo   %EMU_A%
echo   then run this again.
echo.
pause
exit /b 1

:copyfail
echo.
echo   Could not copy %EMU_A% to %EMU_B%.
echo.
pause
exit /b 1

:end
endlocal
