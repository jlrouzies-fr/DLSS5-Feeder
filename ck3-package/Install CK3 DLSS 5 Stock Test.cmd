@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0DLSS5-CK3.ps1" -Action Install -Profile DLSS5 -GameRoot "%~dp0." -AcceptDependencyLicenses -AcceptRuntimeLicenses
set "CK3_DLSS_EXIT=%ERRORLEVEL%"
echo.
if not "%CK3_DLSS_EXIT%"=="0" echo Setup did not complete. No game launch was attempted.
pause
exit /b %CK3_DLSS_EXIT%
