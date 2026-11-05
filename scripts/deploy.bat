@echo off
REM Copy the freshly built plugin into a P5R install for testing.
REM Usage: scripts\deploy.bat "C:\Path\To\steamapps\common\P5R"
REM Close the game first. ReShade must be disabled (no dxgi.dll in the game folder).
setlocal
set SRC=%~dp0..\builds\MEVR_P5RProbe.asi
set GAME=%~1
if "%GAME%"=="" ( echo usage: deploy.bat "^<P5R install folder^>" & exit /b 1 )
if not exist "%SRC%" ( echo [deploy] build first - %SRC% missing & exit /b 1 )
if not exist "%GAME%\P5R.exe" ( echo [deploy] P5R.exe not found in "%GAME%" & exit /b 1 )

copy /y "%SRC%" "%GAME%\MEVR_P5RProbe.asi" >nul || ( echo [deploy] copy FAILED & exit /b 1 )
if not exist "%GAME%\openxr_loader.dll" copy /y "%~dp0..\release\openxr_loader.dll" "%GAME%\" >nul
if not exist "%GAME%\dinput8.dll" copy /y "%~dp0..\release\dinput8.dll" "%GAME%\" >nul
echo [deploy] OK -^> "%GAME%\MEVR_P5RProbe.asi"
echo [deploy] logs after a run: "%GAME%\p5rprobe_log.txt" and "%GAME%\p5r_xr.log"
