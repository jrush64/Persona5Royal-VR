@echo off
REM Build the Persona 5 Royal VR plugin (x64, static CRT).
REM Output: builds\MEVR_P5RProbe.asi  (loaded by Ultimate ASI Loader's dinput8.dll)
REM Prerequisites: Visual Studio 2022 (Community/Professional/Enterprise or the standalone
REM Build Tools) with the "Desktop development with C++" workload. See BUILD.md.
REM Pass DIAG as the first argument to build the developer diagnostics in (frame recorder,
REM automatic eye-image dumps). Normal builds leave them out.
setlocal
cd /d "%~dp0"

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" set VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" ( echo [build] vswhere.exe not found - is Visual Studio 2022 installed? & exit /b 1 )

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSINSTALL=%%i
if not defined VSINSTALL ( echo [build] no VS install with the C++ toolset found & exit /b 1 )

call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 ( echo [build] vcvarsall failed & exit /b 1 )

set ROOT=%~dp0..
set SRC=%ROOT%\src
set OUTDIR=%ROOT%\builds
set OBJDIR=%ROOT%\builds\obj
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

REM MinHook and Dear ImGui are vendored in this repo under third_party/ - nothing to fetch.
set MH=%ROOT%\third_party\minhook
set IM=%ROOT%\third_party\imgui
if not exist "%MH%\src\hook.c" ( echo [build] minhook not found at "%MH%" & exit /b 1 )
if not exist "%IM%\imgui.cpp" ( echo [build] imgui not found at "%IM%" & exit /b 1 )

set DIAGDEF=
if /i "%~1"=="DIAG" ( set DIAGDEF=/DP5R_DIAGNOSTICS & echo [build] developer diagnostics ON )

REM /MT = static CRT so the plugin needs no vcruntime DLL in the game folder.
REM The OpenXR loader is LoadLibrary'd at runtime - no OpenXR SDK or import library needed.
cl /nologo /O2 /MT /LD /EHsc /std:c++17 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS %DIAGDEF% ^
  /I"%MH%\include" /I"%IM%" /Fo"%OBJDIR%\\" ^
  "%SRC%\p5r_probe.cpp" "%SRC%\p5r_xr.cpp" "%SRC%\p5r_gfdcam.cpp" "%SRC%\p5r_menu.cpp" ^
  "%IM%\imgui.cpp" "%IM%\imgui_draw.cpp" "%IM%\imgui_tables.cpp" "%IM%\imgui_widgets.cpp" "%IM%\backends\imgui_impl_dx11.cpp" ^
  "%MH%\src\buffer.c" "%MH%\src\hook.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" ^
  /Fe:"%OUTDIR%\MEVR_P5RProbe.asi" /link user32.lib d3d11.lib dxgi.lib d3dcompiler.lib /MAP:"%OUTDIR%\MEVR_P5RProbe.map"
if errorlevel 1 ( echo [build] compile/link FAILED & exit /b 1 )

for %%F in ("%OUTDIR%\MEVR_P5RProbe.asi") do echo [build] OK -^> %%~fF  (%%~zF bytes)
endlocal
