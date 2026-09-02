@echo off
rem dlss5-feed.addon32 -- the 32-bit in-game half (no NGX; it lives in the host).
rem
rem MinHook (external\minhook, BSD-2) backs the vkCreateDevice hook in src\feed_vk_hook.h,
rem which the Vulkan/DXVK transport needs. MinHook picks its disassembler from the target
rem architecture, so this build compiles hde\hde32.c where build.bat compiles hde64.c.
rem Objects go to build\x86\ because the MinHook sources are compiled by BOTH builds and
rem would otherwise overwrite each other's .obj with the wrong architecture.
cd /d "%~dp0"
if not exist build mkdir build
if not exist build\x86 mkdir build\x86
setlocal
call "%~dp0tools\vcvars.bat" amd64_x86 || exit /b 1
cl /nologo /LD /EHsc /O2 /MD /W3 /std:c++20 /Iexternal\reshade\include /Iexternal\imgui /Iexternal\vulkan /Iexternal\minhook\include /Fobuild\x86\ /Fdbuild\x86\ ^
   src\dlss5-feed32.cpp ^
   external\minhook\src\buffer.c external\minhook\src\hook.c external\minhook\src\trampoline.c external\minhook\src\hde\hde32.c ^
   /link /OUT:build\dlss5-feed.addon32 d3d11.lib dwmapi.lib kernel32.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1
endlocal
echo addon32 built.
