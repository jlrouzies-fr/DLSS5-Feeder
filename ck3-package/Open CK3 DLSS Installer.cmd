@echo off
setlocal
set "CK3_DLSS_GUI=%~dp0tools\CK3-DLSS-Installer\CK3 DLSS Installer.exe"
if not exist "%CK3_DLSS_GUI%" (
    echo ERROR: The CK3 DLSS Installer application is missing.
    echo Re-extract the complete package or use one of the profile-specific installer commands.
    pause
    exit /b 2
)
start "CK3 DLSS Installer" "%CK3_DLSS_GUI%" --package-root "%~dp0."
exit /b 0
