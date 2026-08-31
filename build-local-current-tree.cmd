@echo off
setlocal
cd /d "%~dp0"

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo ERROR: Visual Studio 2022 Build Tools vcvars64.bat was not found.
    exit /b 2
)

call "%VCVARS%" >nul
if errorlevel 1 exit /b %ERRORLEVEL%
if not exist build mkdir build

rc /nologo /fo build\version.res src\version.rc
if errorlevel 1 exit /b %ERRORLEVEL%

cl /nologo /LD /EHsc /O2 /MD /W3 /std:c++20 ^
   /Iexternal\reshade\include /Iexternal\ngx /Iexternal\vulkan /Iexternal\imgui /Iexternal\minhook\include ^
   /Fobuild\ /Fdbuild\ ^
   src\dlss5-feed.cpp ^
   external\minhook\src\buffer.c external\minhook\src\hook.c external\minhook\src\trampoline.c external\minhook\src\hde\hde64.c ^
   /link /OUT:build\dlss5-feed.addon64 build\version.res external\ngx\libs\nvsdk_ngx_d.lib ^
   version.lib kernel32.lib user32.lib advapi32.lib ole32.lib

exit /b %ERRORLEVEL%
