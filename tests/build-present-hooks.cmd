@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %ERRORLEVEL%
if not exist build\compat-tests mkdir build\compat-tests
cl /nologo /LD /EHsc /O2 /MD /W3 /std:c++20 /Iexternal\reshade\include /Iexternal\ngx /Iexternal\vulkan /Iexternal\imgui /Iexternal\minhook\include /Fobuild\compat-tests\ tests\vk-hook-observer.cpp external\minhook\src\buffer.c external\minhook\src\hook.c external\minhook\src\trampoline.c external\minhook\src\hde\hde64.c /link /OUT:build\compat-tests\vk-hook-observer.addon64 external\ngx\libs\nvsdk_ngx_d.lib version.lib kernel32.lib user32.lib advapi32.lib ole32.lib
if errorlevel 1 exit /b %ERRORLEVEL%
cl /nologo /EHsc /O2 /MD /W3 /std:c++20 /Iexternal\vulkan /Fobuild\compat-tests\ /Febuild\compat-tests\native-streamline-smoke.exe tests\native-streamline-smoke.cpp /link user32.lib
exit /b %ERRORLEVEL%
