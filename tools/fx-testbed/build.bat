@echo off
setlocal
cd /d "%~dp0..\.."
call "%~dp0..\vcvars.bat" x64 || exit /b 1
if not exist build\fxlab mkdir build\fxlab
cl /nologo /EHsc /O2 /MD /W3 /std:c++20 /Fobuild\fxlab\ /Fdbuild\fxlab\ tools\fx-testbed\fxlab.cpp /link /OUT:build\fxlab\fxlab.exe d3d11.lib dxgi.lib d2d1.lib dwrite.lib d3dcompiler.lib windowscodecs.lib ole32.lib user32.lib winmm.lib
if errorlevel 1 exit /b 1
echo fxlab built.
endlocal
