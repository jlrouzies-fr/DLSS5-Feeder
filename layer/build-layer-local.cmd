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
cl /nologo /LD /EHsc /O2 /MD /W3 /std:c++20 /I..\external\vulkan feed_vk_layer.cpp ^
   /Fe:VkLayer_feed_vk.dll ^
   /link /OUT:VkLayer_feed_vk.dll /EXPORT:vkNegotiateLoaderLayerInterfaceVersion kernel32.lib
if errorlevel 1 exit /b %ERRORLEVEL%
cl /nologo /LD /EHsc /O2 /MD /W3 /std:c++20 dxgi_bridge.cpp ^
   /Fe:dxgi.dll ^
   /link /OUT:dxgi.dll /DEF:dxgi_bridge.def kernel32.lib user32.lib dxguid.lib
exit /b %ERRORLEVEL%
