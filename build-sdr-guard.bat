@echo off
setlocal
cd /d "%~dp0"
call "%~dp0tools\vcvars.bat" x64 || exit /b 1
if not exist build mkdir build
cl /nologo /LD /EHsc /O2 /MD /W3 /std:c++20 /Iexternal\reshade\include /Fobuild\ /Fdbuild\ src\sdr-guard.cpp /link /OUT:build\sdr-guard.addon64 kernel32.lib user32.lib dxgi.lib
endlocal
