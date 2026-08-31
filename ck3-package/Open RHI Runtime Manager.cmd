@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0DLSS5-CK3.ps1" -Action OpenRHI -GameRoot "%~dp0."
set "CK3_DLSS_EXIT=%ERRORLEVEL%"
echo.
if not "%CK3_DLSS_EXIT%"=="0" echo RHI Manager was not opened.
pause
exit /b %CK3_DLSS_EXIT%

