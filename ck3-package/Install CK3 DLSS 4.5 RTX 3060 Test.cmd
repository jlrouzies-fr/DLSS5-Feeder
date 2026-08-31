@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0DLSS5-CK3.ps1" -Action Install -Profile DLSS45 -GameRoot "%~dp0." -AcceptDependencyLicenses -AcceptRuntimeLicenses
set "CK3_DLSS_EXIT=%ERRORLEVEL%"
echo.
if not "%CK3_DLSS_EXIT%"=="0" echo DLSS 4.5 test setup did not complete.
pause
exit /b %CK3_DLSS_EXIT%

