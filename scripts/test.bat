@echo off
REM Offline regression tests: native XR-wrapper logic and D3D11 WARP rendering checks.
REM They do not launch the game or verify anything in a headset.
setlocal
cd /d "%~dp0..\src\tests"

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" set VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSINSTALL=%%i
if not defined VSINSTALL ( echo [test] no VS install with the C++ toolset found & exit /b 1 )
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 exit /b 1

set IM=%~dp0..\third_party\imgui
set MH=%~dp0..\third_party\minhook
set CF=/nologo /O2 /MT /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS
set IMSRC="%IM%\imgui.cpp" "%IM%\imgui_draw.cpp" "%IM%\imgui_tables.cpp" "%IM%\imgui_widgets.cpp" "%IM%\backends\imgui_impl_dx11.cpp"
set MHSRC="%MH%\src\buffer.c" "%MH%\src\hook.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c"
set LIBS=user32.lib d3d11.lib dxgi.lib d3dcompiler.lib

cl %CF% xr_isolation_test.cpp /Fe:xr_isolation_test.exe /link %LIBS% || exit /b 1
.\xr_isolation_test.exe || exit /b 1
cl %CF% /I"%MH%\include" /I"%IM%" /DP5R_TEST_CAMERA_CONTEXT context_isolation_test.cpp ..\p5r_xr.cpp ..\p5r_gfdcam.cpp ..\p5r_menu.cpp %IMSRC% %MHSRC% /Fe:context_isolation_test.exe /link %LIBS% || exit /b 1
.\context_isolation_test.exe || exit /b 1
cl %CF% /I"%IM%" menu_test.cpp ..\p5r_gfdcam.cpp %IMSRC% /Fe:menu_test.exe /link %LIBS% || exit /b 1
.\menu_test.exe || exit /b 1
.\menu_test.exe camera || exit /b 1
.\menu_test.exe headset || exit /b 1
cl %CF% player_probe_test.cpp /Fe:player_probe_test.exe /link user32.lib || exit /b 1
.\player_probe_test.exe || exit /b 1
cl %CF% precise_cull_test.cpp /Fe:precise_cull_test.exe || exit /b 1
.\precise_cull_test.exe || exit /b 1
echo [test] all offline tests passed
endlocal
