@echo off
rem Starts Gauntlet Legends: Recompiled (built by SETUP.bat).
setlocal
set "EXE=%~dp0port\build\GauntletRecomp.exe"
if not exist "%EXE%" (
  echo The game is not built yet: run SETUP.bat first.
  pause
  exit /b 1
)
cd /d "%~dp0port\build"
start "" "%EXE%" %*
