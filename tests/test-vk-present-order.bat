@echo off
setlocal
cd /d "%~dp0.."
call tools\vcvars.bat x64 || exit /b 1
if not exist build mkdir build
cl /nologo /EHsc /W4 /std:c++20 /Iexternal\vulkan tests\vk-present-order.cpp /Fobuild\ /Febuild\vk-present-order.exe || exit /b 1
build\vk-present-order.exe
exit /b %errorlevel%
