@echo off
rem ovlab.exe / ovlab64.exe -- the lab game for the in-game panel (see ovlab.cpp), in both
rem bitnesses: a 32-bit game reaches the neural tool through host64's helper, a 64-bit one
rem either the same way (helper mode) or in its own process.
cd /d "%~dp0..\.."
if not exist build\ovlab mkdir build\ovlab
if not exist build\ovlab\obj32 mkdir build\ovlab\obj32
if not exist build\ovlab\obj64 mkdir build\ovlab\obj64

setlocal
call "%~dp0..\vcvars.bat" amd64_x86 || exit /b 1
cl /nologo /EHsc /O2 /MD /W3 /std:c++20 /Fobuild\ovlab\obj32\ /Fdbuild\ovlab\obj32\ tools\overlay-lab\ovlab.cpp /link /OUT:build\ovlab\ovlab.exe d3d11.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib
if errorlevel 1 exit /b 1
endlocal

setlocal
call "%~dp0..\vcvars.bat" x64 || exit /b 1
cl /nologo /EHsc /O2 /MD /W3 /std:c++20 /Fobuild\ovlab\obj64\ /Fdbuild\ovlab\obj64\ tools\overlay-lab\ovlab.cpp /link /OUT:build\ovlab\ovlab64.exe d3d11.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib
if errorlevel 1 exit /b 1
endlocal
echo ovlab built.
