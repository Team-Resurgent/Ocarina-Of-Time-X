@echo off
rem Ocarina-Of-Time-X: one double-click from your ROM to Ocarina-Of-Time-X.iso.
rem
rem   1. put your Ocarina of Time ROM in this folder, named baserom.z64
rem   2. run this file (double-click, or "build.bat" in a console)
rem
rem It finds a Python 3.8 or later and hands over to setup.py, which checks the
rem ROM, fetches Ship of Harkinian's asset definitions and Torch, builds the
rem archive tools, extracts the game from the ROM and builds. Arguments are
rem passed on: build.bat --check, build.bat --tester, build.bat --skip-build.
setlocal EnableExtensions
set "OOTX_SELF=%~f0"
if not defined PATH set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem"
cd /d "%~dp0"
title Ocarina-Of-Time-X build

echo.
echo  Ocarina-Of-Time-X -- Ocarina of Time (Ship of Harkinian) for the original Xbox
echo.

set "OOTX_ROM="
rem --check reports a missing ROM itself, with everything else
if /i "%~1"=="--check" set "OOTX_ROM=check"
for %%R in (baserom.z64 baserom.v64 baserom.n64 oot.z64 oot.v64 oot.n64) do (
  if not defined OOTX_ROM if exist "%%R" set "OOTX_ROM=%%R"
)
if not defined OOTX_ROM (
  echo  Your ROM is not here. Copy your Ocarina of Time ROM into
  echo      %~dp0
  echo  and name it  baserom.z64  then run this again.
  goto :fail
)

set "OOTX_PY="
for %%V in (3.13 3.12 3.14 3.15 3.11 3.10 3.9 3.8) do (
  if not defined OOTX_PY (
    py -%%V -c "import sys" >nul 2>nul && set "OOTX_PY=py -%%V"
  )
)
if not defined OOTX_PY (
  python -c "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)" >nul 2>nul && set "OOTX_PY=python"
)
if not defined OOTX_PY (
  echo  No Python 3.8 or later was found. Install it from
  echo      https://www.python.org/downloads/
  echo  ^(tick "Add python.exe to PATH"^), then run this again.
  goto :fail
)

echo  Python: %OOTX_PY%
echo.
%OOTX_PY% setup.py %*
set "OOTX_RC=%ERRORLEVEL%"
if not "%OOTX_RC%"=="0" goto :failrc
echo.
echo  Done.
call :pause
exit /b 0

:failrc
echo.
echo  The build stopped (exit code %OOTX_RC%). The messages above say why.
call :pause
exit /b %OOTX_RC%

:fail
echo.
call :pause
exit /b 1

rem ---- keep a double-clicked window open --------------------------------
:pause
if defined OOTX_NONINTERACTIVE exit /b 0
echo %cmdcmdline% | find /i "%OOTX_SELF%" >nul
if not errorlevel 1 pause
exit /b 0
