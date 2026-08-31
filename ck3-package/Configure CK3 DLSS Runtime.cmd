@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0DLSS5-CK3.ps1" -Action ConfigureRuntime -GameRoot "%~dp0."
set "CK3_DLSS_EXIT=%ERRORLEVEL%"
echo.
if not "%CK3_DLSS_EXIT%"=="0" echo The runtime profile was not changed.
pause
exit /b %CK3_DLSS_EXIT%

