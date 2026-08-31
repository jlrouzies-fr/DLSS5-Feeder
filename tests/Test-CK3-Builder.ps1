[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$repoRoot = Split-Path -Parent $PSScriptRoot
$releaseRoot = Join-Path $repoRoot 'release'
$thinOutput = Join-Path $releaseRoot 'test-ck3-bootstrap'
$offlineOutput = Join-Path $releaseRoot 'test-ck3-offline'
$partialOutput = Join-Path $releaseRoot 'test-ck3-partial'
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('ck3-builder-tests-' + [guid]::NewGuid().ToString('N'))

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

function Ensure-Directory([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { New-Item -ItemType Directory -Path $Path -Force | Out-Null }
}

function New-FakeX64Pe([string]$Path, [string[]]$Markers = @()) {
    Ensure-Directory (Split-Path -Parent $Path)
    $bytes = [byte[]]::new(16384)
    $bytes[0] = 0x4D
    $bytes[1] = 0x5A
    [BitConverter]::GetBytes([int]0x80).CopyTo($bytes, 0x3C)
    $bytes[0x80] = 0x50
    $bytes[0x81] = 0x45
    [BitConverter]::GetBytes([uint16]0x8664).CopyTo($bytes, 0x84)
    $offset = 1024
    foreach ($marker in $Markers) {
        $encoded = [Text.Encoding]::ASCII.GetBytes($marker)
        $encoded.CopyTo($bytes, $offset)
        $offset += $encoded.Length + 1
    }
    [IO.File]::WriteAllBytes($Path, $bytes)
}

function Write-Utf8([string]$Path, [string]$Text) {
    Ensure-Directory (Split-Path -Parent $Path)
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function New-ZipFromDirectory([string]$Source, [string]$Destination) {
    if (Test-Path -LiteralPath $Destination) { Remove-Item -LiteralPath $Destination -Force }
    [IO.Compression.ZipFile]::CreateFromDirectory($Source, $Destination)
}

function Assert-SafeTestOutput([string]$Path) {
    $resolved = [IO.Path]::GetFullPath($Path)
    $expectedRoot = [IO.Path]::GetFullPath($releaseRoot).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($expectedRoot, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notlike 'test-ck3-*') {
        throw "Refusing to remove unexpected test output '$resolved'."
    }
}

try {
    Ensure-Directory $tempRoot
    $inputs = Join-Path $tempRoot 'inputs'
    Ensure-Directory $inputs

    $feeder = Join-Path $inputs 'dlss5-feed.addon64'
    New-FakeX64Pe $feeder @('standalone DLSS neural reconstruction', 'L - DLSS 4.5 (Ultra Performance tuned)')
    $dlss45 = Join-Path $inputs 'dlss45\nvngx_dlss.dll'
    $dlss5 = Join-Path $inputs 'dlss5\nvngx_dlss.dll'
    $dlss5Nr = Join-Path $inputs 'dlss5\nvngx_dlssnr.dll'
    $extended = Join-Path $inputs 'extended\nvngx_dlss.dll'
    $extendedNr = Join-Path $inputs 'extended\nvngx_dlssnr.dll'
    $reno = Join-Path $inputs 'renodx-dlss5.addon64'
    foreach ($path in @($dlss45, $dlss5, $dlss5Nr, $extended, $extendedNr, $reno)) { New-FakeX64Pe $path }

    $layerSource = Join-Path $tempRoot 'layer-source'
    New-FakeX64Pe (Join-Path $layerSource 'VkLayer_feed_vk.dll')
    Write-Utf8 (Join-Path $layerSource 'VkLayer_feed_vk.json') '{"layer":{"name":"VK_LAYER_feed_vk"}}'
    $layerZip = Join-Path $inputs 'feed-vk-layer.zip'
    New-ZipFromDirectory $layerSource $layerZip

    $common = @{
        FeederAddon = $feeder
        FeedLayerZip = $layerZip
        KeepStagingDirectory = $true
    }

    Write-Host 'TEST: public bootstrap excludes third-party graphics and proprietary runtime files'
    $thinArguments = $common.Clone()
    $thinArguments.OutputDirectory = $thinOutput
    & (Join-Path $repoRoot 'Build-CK3-Package.ps1') @thinArguments
    Assert-True (Test-Path -LiteralPath "$thinOutput.zip" -PathType Leaf) 'Bootstrap release ZIP was not created.'
    Assert-True (Test-Path -LiteralPath (Join-Path $thinOutput 'binaries\dlss-payload\dlss5-feed.addon64') -PathType Leaf) 'Fork feeder payload is missing.'
    Assert-True (Test-Path -LiteralPath (Join-Path $thinOutput 'Graphics-Dependency-Setup.ps1') -PathType Leaf) 'Graphics bootstrap is missing.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $thinOutput 'binaries\dlss5-vulkan\ReShade64.dll') -PathType Leaf)) 'Bootstrap unexpectedly redistributes ReShade64.dll.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $thinOutput 'binaries\reshade-shaders\Shaders\ReShade.fxh') -PathType Leaf)) 'Bootstrap unexpectedly redistributes ReShade.fxh.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $thinOutput 'binaries\third-party\vort_Shaders') -PathType Container)) 'Bootstrap unexpectedly redistributes VORT.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $thinOutput 'binaries\dlss-payload\runtimes\DLSS45\nvngx_dlss.dll') -PathType Leaf)) 'Bootstrap unexpectedly contains DLSS.'
    foreach ($license in @('DLSS5-Feeder-LICENSE.txt', 'MinHook-LICENSE.txt', 'Dear-ImGui-LICENSE.txt', 'ReShade-LICENSE.txt', 'NIGos-bridge-LICENSE.txt')) {
        Assert-True (Test-Path -LiteralPath (Join-Path $thinOutput "THIRD-PARTY-LICENSES\$license") -PathType Leaf) "License bundle is missing $license."
    }
    $provenance = Get-Content -LiteralPath (Join-Path $thinOutput 'BUILD-PROVENANCE.txt') -Raw
    Assert-True ($provenance -match 'maintainer-supplied CK3 fork build') 'Provenance does not identify the fork build.'
    Assert-True ($provenance -match 'ReShade=not bundled') 'Provenance does not record first-run acquisition.'

    Write-Host 'TEST: private offline inputs are staged in unambiguous profile directories'
    $offlineArguments = $common.Clone()
    $offlineArguments.OutputDirectory = $offlineOutput
    $offlineArguments.Dlss45Runtime = $dlss45
    $offlineArguments.Dlss5Runtime = $dlss5
    $offlineArguments.Dlss5NrRuntime = $dlss5Nr
    $offlineArguments.Dlss5ExtendedRuntime = $extended
    $offlineArguments.Dlss5ExtendedNrRuntime = $extendedNr
    $offlineArguments.RenoDxAddon = $reno
    $offlineArguments.AcknowledgeRuntimeRedistributionTerms = $true
    $offlineArguments.AllowUnsignedTestArtifacts = $true
    & (Join-Path $repoRoot 'Build-CK3-Package.ps1') @offlineArguments
    foreach ($relative in @(
        'DLSS45\nvngx_dlss.dll',
        'DLSS5\nvngx_dlss.dll',
        'DLSS5\nvngx_dlssnr.dll',
        'DLSS5Extended\nvngx_dlss.dll',
        'DLSS5Extended\nvngx_dlssnr.dll',
        'shared\renodx-dlss5.addon64'
    )) {
        Assert-True (Test-Path -LiteralPath (Join-Path $offlineOutput "binaries\dlss-payload\runtimes\$relative") -PathType Leaf) "Offline payload is missing $relative."
    }

    Write-Host 'TEST: a partial NR pair is rejected before a ZIP is produced'
    $partialArguments = $common.Clone()
    $partialArguments.OutputDirectory = $partialOutput
    $partialArguments.Dlss5NrRuntime = $dlss5Nr
    $partialArguments.AcknowledgeRuntimeRedistributionTerms = $true
    $partialArguments.AllowUnsignedTestArtifacts = $true
    $rejected = $false
    try { & (Join-Path $repoRoot 'Build-CK3-Package.ps1') @partialArguments }
    catch { $rejected = $_.Exception.Message -match 'requires both' }
    Assert-True $rejected 'The builder accepted an ambiguous partial DLSS/NR pair.'

    Write-Host 'All isolated CK3 builder tests passed.' -ForegroundColor Green
}
finally {
    foreach ($output in @($thinOutput, $offlineOutput, $partialOutput)) {
        Assert-SafeTestOutput $output
        if (Test-Path -LiteralPath $output -PathType Container) { Remove-Item -LiteralPath $output -Recurse -Force }
        if (Test-Path -LiteralPath "$output.zip" -PathType Leaf) { Remove-Item -LiteralPath "$output.zip" -Force }
    }
    if (Test-Path -LiteralPath $tempRoot -PathType Container) { Remove-Item -LiteralPath $tempRoot -Recurse -Force }
}

