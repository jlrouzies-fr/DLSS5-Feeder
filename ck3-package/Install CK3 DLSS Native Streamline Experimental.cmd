@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0DLSS5-CK3.ps1" -Action Install -Profile NativeStreamline -GameRoot "%~dp0." -AcceptDependencyLicenses -AcceptRuntimeLicenses
if errorlevel 1 pause
endlocal
