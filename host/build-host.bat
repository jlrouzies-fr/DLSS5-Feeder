@echo off
rem dlss5-feed-host64.exe -- the 64-bit NGX host for 32-bit games.
cd /d "%~dp0"
setlocal
call "%~dp0..\tools\vcvars.bat" x64 || exit /b 1
cl /nologo /O2 /EHsc /W3 /MD /I..\external\ngx dlss5-feed-host64.cpp ^
   /Fe:dlss5-feed-host64.exe ^
   /link ..\external\ngx\libs\nvsdk_ngx_d.lib version.lib winmm.lib kernel32.lib user32.lib gdi32.lib advapi32.lib ole32.lib
if errorlevel 1 exit /b 1
endlocal
echo host built.
