@echo off
setlocal enableextensions enabledelayedexpansion
title Persona 5 Royal VR
color 0C

echo(
echo  --------------------------------------------------
echo    Persona 5 Royal VR  ^(work in progress^)
echo  --------------------------------------------------
echo(

set "TARGET=%~dp0"
if "!TARGET:~-1!"=="\" set "TARGET=!TARGET:~0,-1!"

if not exist "!TARGET!\P5R.exe" (
  echo   ERROR: P5R.exe is not next to this installer.
  echo(
  echo   This bat has to run FROM your game folder, not from the zip or
  echo   your Downloads folder. Fix:
  echo(
  echo     1. Extract the whole zip.
  echo     2. Copy ALL its files into the folder that has P5R.exe.
  echo        ^(Steam: right-click Persona 5 Royal -^> Manage -^> Browse Local Files^)
  echo     3. Run P5R-VR.bat from THAT folder.
  echo(
  pause
  exit /b 1
)

REM --- game must be closed before touching its files ---
tasklist /fi "imagename eq P5R.exe" 2>nul | "%SystemRoot%\System32\find.exe" /i "P5R.exe" >nul
if not errorlevel 1 (
  echo   Please CLOSE Persona 5 Royal first, then run this again.
  echo(
  pause
  exit /b 1
)

REM --- write-access check; relaunch elevated once if the folder needs admin rights ---
set "WTEST=!TARGET!\.p5rvr_writetest"
(echo test) > "!WTEST!" 2>nul
if not exist "!WTEST!" (
  net session >nul 2>&1
  if errorlevel 1 (
    echo   This folder needs Administrator rights to change.
    echo   Requesting permission - click YES on the popup...
    echo(
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -WorkingDirectory '%~dp0' -Verb RunAs" >nul 2>&1
    exit /b 0
  ) else (
    echo   ERROR: cannot write to this folder even as Administrator.
    echo   Check it isn't read-only, or that antivirus isn't blocking it.
    echo(
    pause
    exit /b 1
  )
)
del /f /q "!WTEST!" >nul 2>&1

set "INSTALLED=0"
if exist "!TARGET!\MEVR_P5RProbe.asi" set "INSTALLED=1"

echo   Game folder:
echo     !TARGET!
echo(
if "!INSTALLED!"=="1" ( echo   Persona 5 Royal VR files are present here. ) else ( echo   Persona 5 Royal VR is NOT installed here. )
echo(
echo   What do you want to do?
echo     [1] Install / check Persona 5 Royal VR
echo     [2] Uninstall Persona 5 Royal VR
echo     [3] Cancel  ^(do nothing^)
echo(
set /p "ACTION=Enter 1-3 [1]: "
if not defined ACTION set "ACTION=1"
if "!ACTION!"=="3" ( echo   Cancelled. Nothing was changed. & pause & exit /b 0 )
if "!ACTION!"=="2" goto UNINSTALL
goto INSTALL


:INSTALL
echo(
set "MISSING="
for %%F in (MEVR_P5RProbe.asi dinput8.dll openxr_loader.dll) do (
  if not exist "!TARGET!\%%F" (
    echo   MISSING: %%F
    set "MISSING=1"
  )
)
if defined MISSING (
  echo(
  echo   Copy ALL the files from the zip into this folder, then run this again.
  echo(
  pause
  exit /b 1
)
echo   Mod files present: MEVR_P5RProbe.asi, dinput8.dll, openxr_loader.dll

REM --- ReShade (dxgi.dll) breaks the mod: park it under a new name, restorable on uninstall ---
if exist "!TARGET!\dxgi.dll" (
  echo(
  echo   Found dxgi.dll in the game folder - that is usually ReShade, and the
  echo   VR mod does not work with ReShade active.
  set /p "RS=Disable it now? It is renamed, not deleted. [Y/N] "
  if /i "!RS!"=="Y" (
    move /y "!TARGET!\dxgi.dll" "!TARGET!\dxgi.dll.p5rvr-disabled" >nul
    echo   Renamed dxgi.dll to dxgi.dll.p5rvr-disabled.
  ) else (
    echo   Left dxgi.dll in place. Expect the mod not to work until it is removed.
  )
)

echo(
echo  --------------------------------------------------
echo    DONE. Persona 5 Royal VR is installed.
echo  --------------------------------------------------
echo(
echo    1. Start your VR runtime, put the headset on.
echo    2. Launch Persona 5 Royal from Steam.
echo    3. INSERT = mod menu,  HOME = recenter,  F5 / hold R3 = first person.
echo(
echo    This is an unfinished build - see README.txt for known problems.
echo    Run this script again any time to uninstall.
echo(
pause
exit /b 0


:UNINSTALL
if "!INSTALLED!"=="0" if not exist "!TARGET!\dxgi.dll.p5rvr-disabled" ( echo   Nothing to uninstall. & pause & exit /b 0 )
echo(
set /p "GO=Uninstall Persona 5 Royal VR? [Y/N] "
if /i not "!GO!"=="Y" ( echo   Cancelled. Nothing was changed. & pause & exit /b 0 )

del /f /q "!TARGET!\MEVR_P5RProbe.asi" >nul 2>&1
del /f /q "!TARGET!\openxr_loader.dll" >nul 2>&1
echo   Removed MEVR_P5RProbe.asi and openxr_loader.dll.

REM --- dinput8.dll is a general ASI loader: only remove it if no other .asi plugins remain ---
set "OTHERASI="
for %%A in ("!TARGET!\*.asi") do set "OTHERASI=1"
if exist "!TARGET!\dinput8.dll" (
  if defined OTHERASI (
    echo   Kept dinput8.dll - other .asi plugins in this folder still use it.
  ) else (
    set /p "DI=Also remove dinput8.dll, the ASI loader? Say N if other mods need it. [Y/N] "
    if /i "!DI!"=="Y" ( del /f /q "!TARGET!\dinput8.dll" >nul 2>&1 & echo   Removed dinput8.dll. )
  )
)

if exist "!TARGET!\dxgi.dll.p5rvr-disabled" (
  if not exist "!TARGET!\dxgi.dll" (
    move /y "!TARGET!\dxgi.dll.p5rvr-disabled" "!TARGET!\dxgi.dll" >nul
    echo   Restored your dxgi.dll ^(ReShade^).
  )
)

echo(
set /p "DELS=Also delete the mod's settings, logs and debug images? [Y/N] "
if /i "!DELS!"=="Y" (
  del /f /q "!TARGET!\p5r_camera.ini" "!TARGET!\p5rprobe_log.txt" "!TARGET!\p5r_xr.log" >nul 2>&1
  del /f /q "!TARGET!\p5r_heartbeat.txt" "!TARGET!\p5r_hb_present.txt" >nul 2>&1
  del /f /q "!TARGET!\p5r_*.bmp" >nul 2>&1
  for /d %%D in ("!TARGET!\p5r_flash_*" "!TARGET!\p5r_effect_*") do rmdir /s /q "%%~D" >nul 2>&1
  echo   Deleted settings, logs and debug images.
) else (
  echo   Kept p5r_camera.ini so your settings survive a reinstall.
)

echo(
echo   Persona 5 Royal VR uninstalled. P5R-VR.bat and README.txt can be deleted by hand.
echo(
pause
exit /b 0
