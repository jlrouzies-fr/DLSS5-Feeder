@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %ERRORLEVEL%
if not exist build\compat-tests mkdir build\compat-tests
cl /nologo /EHsc /O2 /MD /W3 /std:c++20 /Iexternal\reshade\include /Iexternal\ngx /Iexternal\vulkan /Iexternal\imgui /Iexternal\minhook\include /Fobuild\compat-tests\ /Febuild\compat-tests\feeder-compat.exe tests\feeder-compat.cpp external\minhook\src\buffer.c external\minhook\src\hook.c external\minhook\src\trampoline.c external\minhook\src\hde\hde64.c /link external\ngx\libs\nvsdk_ngx_d.lib version.lib kernel32.lib user32.lib advapi32.lib ole32.lib
if errorlevel 1 exit /b %ERRORLEVEL%
build\compat-tests\feeder-compat.exe
if errorlevel 1 exit /b %ERRORLEVEL%
cl /nologo /EHsc /W4 /std:c++20 /Iexternal\vulkan tests\vk-present-order.cpp /Fobuild\compat-tests\ /Febuild\compat-tests\vk-present-order.exe
if errorlevel 1 exit /b %ERRORLEVEL%
build\compat-tests\vk-present-order.exe
exit /b %ERRORLEVEL%