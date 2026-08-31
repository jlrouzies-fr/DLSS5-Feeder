@echo off
setlocal
set "CK3_ROOT=%~dp0."
set "CK3_BIN=%~dp0binaries"
set "CK3_DLSS_LAYER_PATH=%CK3_BIN%\dlss5-vulkan"

if not exist "%CK3_BIN%\ck3.exe" (
    echo ERROR: binaries\ck3.exe was not found.
    echo Extract this package into the Crusader Kings III folder, not the binaries folder.
    pause
    exit /b 2
)

if not exist "%CK3_BIN%\dlss-active\CK3-DLSS-RUNTIME.json" (
    echo ERROR: No active runtime profile exists.
    echo Run "Install CK3 DLSS.cmd" first.
    pause
    exit /b 3
)

if not exist "%CK3_BIN%\dlss-active\dlss5-feed.addon64" (
    echo ERROR: The active DLSS feeder is missing. Run "Install CK3 DLSS.cmd" again.
    pause
    exit /b 4
)

rem Explicit layers keep this install portable: no Vulkan registry keys and no effect on other games.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0DLSS5-CK3.ps1" -Action Validate -GameRoot "%CK3_ROOT%"
if errorlevel 1 (
    echo.
    echo ERROR: CK3 DLSS validation failed. Run "Install CK3 DLSS.cmd" again.
    pause
    exit /b 5
)

set "VK_LAYER_PATH=%CK3_DLSS_LAYER_PATH%"
set "VK_INSTANCE_LAYERS=VK_LAYER_feed_vk;VK_LAYER_reshade"
set "DISABLE_VK_LAYER_reshade_1=1"
set "RESHADE_BASE_PATH_OVERRIDE=%CK3_BIN%"

pushd "%CK3_BIN%"
start "Crusader Kings III - CK3 DLSS" "ck3.exe" -gdpr-compliant %*
popd
endlocal

