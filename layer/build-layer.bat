@echo off
rem VkLayer_feed_vk.dll -- appends the external-interop extensions the feeder needs.
cd /d "%~dp0"
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /LD /EHsc /O2 /MD /W3 /std:c++20 /I..\external\vulkan feed_vk_layer.cpp ^
   /Fe:VkLayer_feed_vk.dll ^
   /link /OUT:VkLayer_feed_vk.dll /EXPORT:vkNegotiateLoaderLayerInterfaceVersion kernel32.lib
if errorlevel 1 exit /b 1
cl /nologo /LD /EHsc /O2 /MD /W3 /std:c++20 dxgi_bridge.cpp ^
   /Fe:dxgi.dll ^
   /link /OUT:dxgi.dll /DEF:dxgi_bridge.def kernel32.lib user32.lib dxguid.lib
if errorlevel 1 exit /b 1
endlocal
echo Vulkan layer and app-local DXGI bridge built.
