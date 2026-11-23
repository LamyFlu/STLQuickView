@echo off
setlocal
call "%STLQV_VCVARS%" >nul
if errorlevel 1 exit /b 1
if not exist build mkdir build
if not exist dist mkdir dist
set "opts=/nologo /std:c++20 /EHsc /O2 /Gy /MT /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Ibuild"
set "fxc=fxc /nologo /O3 /Qstrip_reflect /Qstrip_debug"
%fxc% /T vs_4_0 /E MeshVS /Vn MeshVSCode /Fh build\shader_mesh_vs.h shaders.hlsl >nul
if errorlevel 1 exit /b 1
%fxc% /T ps_4_0 /E MeshPS /Vn MeshPSCode /Fh build\shader_mesh_ps.h shaders.hlsl >nul
if errorlevel 1 exit /b 1
%fxc% /T vs_4_0 /E ColorVS /Vn ColorVSCode /Fh build\shader_color_vs.h shaders.hlsl >nul
if errorlevel 1 exit /b 1
%fxc% /T ps_4_0 /E ColorPS /Vn ColorPSCode /Fh build\shader_color_ps.h shaders.hlsl >nul
if errorlevel 1 exit /b 1
cl %opts% /c /Fo:build\ mesh.cpp formats.cpp viewer.cpp render.cpp render_d3d11.cpp render_gl.cpp shell.cpp
if errorlevel 1 exit /b 1
rc /nologo /i build /fo build\viewer.res viewer.rc
if errorlevel 1 exit /b 1
rc /nologo /i build /fo build\shell.res shell.rc
if errorlevel 1 exit /b 1
link /nologo /SUBSYSTEM:WINDOWS /OUT:dist\StlQuickView.exe /IMPLIB:build\StlQuickView.lib build\viewer.obj build\formats.obj build\render.obj build\render_d3d11.obj build\render_gl.obj build\mesh.obj build\viewer.res user32.lib gdi32.lib shell32.lib comdlg32.lib opengl32.lib advapi32.lib gdiplus.lib d3d11.lib dxgi.lib d2d1.lib dwrite.lib
if errorlevel 1 exit /b 1
link /nologo /DLL /DEF:shell.def /OUT:dist\StlQuickViewShell.dll /IMPLIB:build\StlQuickViewShell.lib build\shell.obj build\formats.obj build\mesh.obj build\shell.res user32.lib gdi32.lib comctl32.lib ole32.lib uuid.lib advapi32.lib windowscodecs.lib
if errorlevel 1 exit /b 1
copy /y Setup.ps1 dist\Setup.ps1 >nul
if errorlevel 1 exit /b 1
copy /y README.md dist\README.md >nul
if errorlevel 1 exit /b 1
if not "%~1"=="installer" exit /b 0
cl %opts% /LD /Fo:build\installer-actions.obj installer\actions.cpp /link /nologo /OUT:build\StlQuickViewInstallerActions.dll /IMPLIB:build\StlQuickViewInstallerActions.lib shell32.lib
