@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0DLSS5-CK3.ps1" -Action Disable -GameRoot "%~dp0."
set "CK3_DLSS_EXIT=%ERRORLEVEL%"
echo.
pause
exit /b %CK3_DLSS_EXIT%

