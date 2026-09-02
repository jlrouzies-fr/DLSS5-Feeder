@echo off
rem Phase-0 spikes: the D3D11/D3D12 cross-process pair, the OpenGL interop pair, and
rem the 32-bit Vulkan interop pair.
rem Spikes may link opengl32.lib -- there is no ReShade in their processes. None of
rem them links vulkan-1.lib: src\feed_vk.h is VK_NO_PROTOTYPES, so the Vulkan pair
rem resolves everything through vkGetInstanceProcAddr, exactly as the add-on does.
cd /d "%~dp0"

setlocal
call "%~dp0..\tools\vcvars.bat" x64 || exit /b 1
cl /nologo /O2 /EHsc /W3 spike-host64.cpp /Fe:spike-host64.exe d3d12.lib dxgi.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /W3 spike-gl64.cpp /Fe:spike-gl64.exe d3d12.lib dxgi.lib opengl32.lib gdi32.lib user32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /W3 spike-vkhost64.cpp /Fe:spike-vkhost64.exe d3d12.lib dxgi.lib
if errorlevel 1 exit /b 1
rem The proxy-swapchain contract (PLAN-PROXY-SWAPCHAIN.md); reuses src\feed_fsr1.h for the upscale.
cl /nologo /O2 /EHsc /W3 /I..\src spike-proxy-swapchain.cpp /Fe:spike-proxy-swapchain.exe d3d11.lib dxgi.lib d3dcompiler.lib user32.lib
if errorlevel 1 exit /b 1
endlocal

setlocal
call "%~dp0..\tools\vcvars.bat" amd64_x86 || exit /b 1
cl /nologo /O2 /EHsc /W3 spike-client32.cpp /Fe:spike-client32.exe d3d11.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /W3 spike-gl32.cpp /Fe:spike-gl32.exe opengl32.lib gdi32.lib user32.lib
if errorlevel 1 exit /b 1
rem Compiling ..\src\feed_vk.h as x86 is itself one of the phase-0 answers.
cl /nologo /O2 /EHsc /W3 /std:c++20 /I..\external\vulkan spike-vkclient32.cpp /Fe:spike-vkclient32.exe
if errorlevel 1 exit /b 1
endlocal

echo spike built.
