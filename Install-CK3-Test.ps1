[CmdletBinding()]
param(
    [string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Crusader Kings III',
    [ValidateSet('DLSS45', 'DLSS5', 'DLSS5Extended')]
    [string]$Profile = 'DLSS45',
    [string]$SettingsPath,
    [switch]$ForceDownload
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$resolvedGame = [IO.Path]::GetFullPath($GameRoot.TrimEnd('\', '/'))
if (-not (Test-Path -LiteralPath (Join-Path $resolvedGame 'binaries\ck3.exe') -PathType Leaf)) {
    throw "CK3 was not found at '$resolvedGame'. Pass -GameRoot with the folder containing binaries\ck3.exe."
}

$temp = Join-Path ([IO.Path]::GetTempPath()) ('ck3-dlss-private-test-' + [guid]::NewGuid().ToString('N'))
$output = Join-Path $PSScriptRoot 'release\CK3-DLSS-Private-Test'
New-Item -ItemType Directory -Path $temp -Force | Out-Null
try {
    $feeder = Join-Path $temp 'dlss5-feed.addon64'
    $layer = Join-Path $temp 'feed-vk-layer.zip'
    Write-Host '[CK3 DLSS test] Downloading the upstream feeder test binary...'
    Invoke-WebRequest -Uri 'https://github.com/jlrouzies-fr/DLSS5-Feeder/releases/latest/download/dlss5-feed.addon64' `
        -OutFile $feeder -UseBasicParsing -Headers @{ 'User-Agent' = 'CK3-DLSS-Private-Test' }
    Write-Host '[CK3 DLSS test] Downloading the feeder Vulkan layer...'
    Invoke-WebRequest -Uri 'https://github.com/jlrouzies-fr/DLSS5-Feeder/releases/latest/download/feed-vk-layer.zip' `
        -OutFile $layer -UseBasicParsing -Headers @{ 'User-Agent' = 'CK3-DLSS-Private-Test' }

    & (Join-Path $PSScriptRoot 'Build-CK3-Package.ps1') `
        -FeederAddon $feeder `
        -FeedLayerZip $layer `
        -OutputDirectory $output `
        -AllowUpstreamFeederForTesting `
        -KeepStagingDirectory

    Write-Host "[CK3 DLSS test] Deploying the test package to '$resolvedGame'..."
    Get-ChildItem -LiteralPath $output -Force | Copy-Item -Destination $resolvedGame -Recurse -Force

    $arguments = @{
        Action = 'Install'
        Profile = $Profile
        GameRoot = $resolvedGame
        AcceptDependencyLicenses = $true
        AcceptRuntimeLicenses = $true
    }
    if ($SettingsPath) { $arguments.SettingsPath = $SettingsPath }
    if ($ForceDownload) { $arguments.ForceDownload = $true }
    & (Join-Path $resolvedGame 'DLSS5-CK3.ps1') @arguments
    if ($LASTEXITCODE -ne 0) { throw "The deployed CK3 installer exited with code $LASTEXITCODE." }

    Write-Host ''
    Write-Host "Private CK3 test installation completed with profile $Profile." -ForegroundColor Green
    Write-Host "Launch it with: $resolvedGame\Launch CK3 with DLSS.cmd"
}
finally {
    if (Test-Path -LiteralPath $temp -PathType Container) { Remove-Item -LiteralPath $temp -Recurse -Force }
}

