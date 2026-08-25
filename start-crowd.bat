@echo off
rem Start the CROWD server and the mixer together.
rem
rem Put this next to vj-mix-spike1.exe, with the crowd-server folder beside
rem it. Edit the two SET lines below if your ring names differ.
rem
rem Order matters only a little: the mixer binds the UDP port it listens on,
rem so if you start it second and the port is taken, it says so in the CROWD
rem section of the Controls window instead of failing silently.

setlocal
cd /d "%~dp0"

set RING_A=Local\vj-mix-prim-A
set RING_B=

where node >nul 2>nul
if errorlevel 1 (
  echo.
  echo   Node was not found on PATH, so the audience feature cannot start.
  echo   Install Node from https://nodejs.org/ ^(LTS is fine^), or run the
  echo   mixer on its own without --crowd.
  echo.
  pause
  exit /b 1
)

if not exist "crowd-server\server.js" (
  echo.
  echo   crowd-server\server.js was not found next to this script.
  echo   Copy the whole crowd-server folder here and try again.
  echo.
  pause
  exit /b 1
)

echo Starting the CROWD server...
start "CROWD server" cmd /k node crowd-server\server.js

rem Give it a moment to bind its ports before the mixer starts asking.
timeout /t 2 /nobreak >nul

echo Starting the mixer...
if "%RING_B%"=="" (
  start "" vj-mix-spike1.exe --crowd --attach-a "%RING_A%"
) else (
  start "" vj-mix-spike1.exe --crowd --attach-a "%RING_A%" --attach-b "%RING_B%"
)

echo.
echo   Both started. The CROWD server window shows the URL to give the room.
echo   Close that window to stop the audience feature; the mixer keeps going.
echo.
endlocal
