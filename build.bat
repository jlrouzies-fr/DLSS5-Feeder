@echo off
setlocal
cd /d "%~dp0"
call "%~dp0tools\vcvars.bat" x64 || exit /b 1
if not exist build mkdir build
rc /nologo /fo build\version.res src\version.rc
rem MinHook (external\minhook, BSD-2) backs the vkCreateDevice hook in src\feed_vk_hook.h.
cl /nologo /LD /EHsc /O2 /MD /W3 /std:c++20 /Iexternal\reshade\include /Iexternal\ngx /Iexternal\vulkan /Iexternal\imgui /Iexternal\minhook\include /Fobuild\ /Fdbuild\ src\dlss5-feed.cpp external\minhook\src\buffer.c external\minhook\src\hook.c external\minhook\src\trampoline.c external\minhook\src\hde\hde64.c /link /OUT:build\dlss5-feed.addon64 build\version.res external\ngx\libs\nvsdk_ngx_d.lib version.lib kernel32.lib user32.lib advapi32.lib ole32.lib d3dcompiler.lib
endlocal
