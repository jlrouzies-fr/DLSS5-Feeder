@echo off
rem 64-bit helper mode: the in-game half (src\dlss5-feed32.cpp) compiled x64, for 64-bit games
rem whose own process cannot run NGX (shadPS4: NvAPI_D3D12_CreateCuModule fails in-process). The
rem game hands its frames to host64\dlss5-feed-host64.exe exactly as a 32-bit game does.
cd /d "%~dp0.."
if not exist build\helper64 mkdir build\helper64
setlocal
call "%~dp0vcvars.bat" x64 || exit /b 1
for /f %%I in ('git rev-parse --short HEAD 2^>nul') do set FEED_BUILD_ID=%%I
if not defined FEED_BUILD_ID set FEED_BUILD_ID=unknown
cl /nologo /LD /EHsc /O2 /MD /W3 /std:c++20 /DFEED_HELPER64 /DFEED_BUILD_ID=\"%FEED_BUILD_ID%-helper64\" /Iexternal\reshade\include /Iexternal\imgui /Iexternal\vulkan /Iexternal\minhook\include /Fobuild\helper64\ /Fdbuild\helper64\ ^
   src\dlss5-feed32.cpp ^
   external\minhook\src\buffer.c external\minhook\src\hook.c external\minhook\src\trampoline.c external\minhook\src\hde\hde64.c ^
   /link /OUT:build\helper64\dlss5-feed-helper.addon64 /IMPLIB:build\helper64\dlss5-feed-helper.lib d3d11.lib dwmapi.lib kernel32.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1
endlocal
echo helper64 built.
