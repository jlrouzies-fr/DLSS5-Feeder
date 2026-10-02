[CmdletBinding()]
param(
    [string]$GraphicsSource,
    [string]$NativeRuntimeDirectory
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $GraphicsSource) { $GraphicsSource = Join-Path $repo 'release/CK3-DLSS-Vulkan-All-Profiles/binaries/dlss5-vulkan' }
if (-not $NativeRuntimeDirectory) { $NativeRuntimeDirectory = Join-Path $repo 'release/CK3-DLSS-Vulkan-All-Profiles/binaries/dlss-payload/runtimes/NativeStreamline' }
$build = Join-Path $repo 'build/compat-tests'
foreach ($name in @('ReShade64.dll', 'ReShade64.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $GraphicsSource $name))) { throw "Missing $name in '$GraphicsSource'. Supply the CK3 package's ReShade 6.8.0 payload." }
}
if ((Get-Item -LiteralPath (Join-Path $GraphicsSource 'ReShade64.dll')).VersionInfo.FileVersion -notlike '6.8.0*') {
    throw 'This regression expects the CK3-pinned ReShade 6.8.0.'
}
if (-not (Test-Path -LiteralPath (Join-Path $NativeRuntimeDirectory 'sl.interposer.dll'))) { throw 'Supply the existing Native Streamline runtime directory.' }
Push-Location $repo
try {
    & .\layer\build-layer-local.cmd
    if ($LASTEXITCODE -ne 0) { throw 'Layer/bridge compilation failed.' }
    & .\tests\build-present-hooks.cmd
    if ($LASTEXITCODE -ne 0) { throw 'Present regression compilation failed.' }
    $envNames = @('VK_LAYER_PATH', 'VK_INSTANCE_LAYERS', 'DISABLE_VK_LAYER_reshade_1')
    $saved = @{}
    foreach ($name in $envNames) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
    try {
        foreach ($mode in @('system', 'native')) {
            $fixture = Join-Path $build "present-hooks-$mode"
            $layers = Join-Path $fixture 'dlss5-vulkan'
            $active = Join-Path $fixture 'dlss-active'
            New-Item -ItemType Directory -Path $layers, $active -Force | Out-Null
            Copy-Item -LiteralPath (Join-Path $GraphicsSource 'ReShade64.dll'), (Join-Path $GraphicsSource 'ReShade64.json') -Destination $layers -Force
            Copy-Item -LiteralPath (Join-Path $repo 'layer/VkLayer_feed_vk.dll'), (Join-Path $repo 'layer/VkLayer_feed_vk.json') -Destination $layers -Force
            Copy-Item -LiteralPath (Join-Path $repo 'layer/dxgi.dll') -Destination $fixture -Force
            Copy-Item -LiteralPath (Join-Path $build 'native-streamline-smoke.exe') -Destination $fixture -Force
            Copy-Item -LiteralPath (Join-Path $build 'vk-hook-observer.addon64') -Destination $active -Force
            [IO.File]::WriteAllText((Join-Path $active 'dlss5-feed.cfg'), "enabled=0`nmode=0`n")
            [IO.File]::WriteAllText((Join-Path $fixture 'ReShade.ini'), "[ADDON]`nAddonPath=.\dlss-active`n[GENERAL]`nEffectSearchPaths=.\no-effects`nPresetPath=.\test-preset.ini`n")
            if ($mode -eq 'native') {
                Get-ChildItem -LiteralPath $NativeRuntimeDirectory -Filter '*.dll' -File | Copy-Item -Destination $active -Force
                [IO.File]::WriteAllText((Join-Path $active 'streamline-native.enabled'), 'test')
            }
            $env:VK_LAYER_PATH = $layers
            $env:VK_INSTANCE_LAYERS = 'VK_LAYER_feed_vk;VK_LAYER_reshade'
            $env:DISABLE_VK_LAYER_reshade_1 = '1'
            $arguments = if ($mode -eq 'system') { @('--system', '--present-hooks') } else { @('--present-hooks') }
            $out = Join-Path $fixture 'smoke.out'
            $err = Join-Path $fixture 'smoke.err'
            Write-Host "TEST: real Vulkan/ReShade presents through $mode path (60 second timeout)"
            $process = Start-Process -FilePath (Join-Path $fixture 'native-streamline-smoke.exe') -ArgumentList $arguments -WorkingDirectory $fixture -WindowStyle Hidden -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
            $null = $process.Handle
            if (-not $process.WaitForExit(60000)) {
                $process.Kill()
                $process.WaitForExit()
                throw "$mode presentation timed out. Inspect logs in '$fixture'."
            }
            Get-Content -LiteralPath $out
            if ((Get-Item -LiteralPath $err).Length -gt 0) { Get-Content -LiteralPath $err }
            if ($process.ExitCode -ne 0) { throw "$mode present regression failed: exit $($process.ExitCode). Inspect logs in '$fixture'." }
        }
    }
    finally {
        foreach ($name in $envNames) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    }
}
finally { Pop-Location }
Write-Host 'CK3 local layer and Native Streamline present hook regressions passed.'
