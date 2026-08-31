[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$FeederAddon,
    [string]$FeedLayerZip,
    [string]$Dlss45Runtime,
    [string]$Dlss5Runtime,
    [string]$Dlss5NrRuntime,
    [string]$Dlss5ExtendedRuntime,
    [string]$Dlss5ExtendedNrRuntime,
    [string]$RenoDxAddon,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'release\CK3-DLSS-Portable'),
    [switch]$AcknowledgeRuntimeRedistributionTerms,
    [switch]$AllowUnsignedTestArtifacts,
    [switch]$AllowUpstreamFeederForTesting,
    [switch]$KeepStagingDirectory
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$script:FeedLayerUrl = 'https://github.com/jlrouzies-fr/DLSS5-Feeder/releases/latest/download/feed-vk-layer.zip'

function Write-Step([string]$Message) {
    Write-Host "[build CK3 package] $Message"
}

function Ensure-Directory([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
}

function Assert-File([string]$Path, [string]$Label, [int64]$MinimumBytes = 1) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Label was not found: '$Path'" }
    $item = Get-Item -LiteralPath $Path
    if ($item.Length -lt $MinimumBytes) { throw "$Label is unexpectedly small ($($item.Length) bytes): '$Path'" }
    return $item.FullName
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-PeMachine([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 4096 -or $reader.ReadUInt16() -ne 0x5A4D) { throw "'$Path' is not a Windows PE file." }
        $stream.Position = 0x3C
        $offset = $reader.ReadInt32()
        if ($offset -lt 0x40 -or ($offset + 6) -gt $stream.Length) { throw "'$Path' has an invalid PE header offset." }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw "'$Path' has an invalid PE signature." }
        return $reader.ReadUInt16()
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Assert-X64Pe([string]$Path, [string]$Label) {
    $resolved = Assert-File $Path $Label 4096
    $machine = Get-PeMachine $resolved
    if ($machine -ne 0x8664) { throw "$Label is not x64 (machine 0x$($machine.ToString('X4'))): '$resolved'" }
    return (Get-Item -LiteralPath $resolved)
}

function Assert-NvidiaPe([string]$Path, [string]$Label, [bool]$AllowModified) {
    $item = Assert-X64Pe $Path $Label
    if (-not $AllowModified) {
        $signature = Get-AuthenticodeSignature -LiteralPath $item.FullName
        $valid = $signature.Status -eq 'Valid' -and $signature.SignerCertificate -and $signature.SignerCertificate.Subject -match '(?i)NVIDIA'
        if (-not $valid -and -not $AllowUnsignedTestArtifacts) {
            throw "$Label must have a valid NVIDIA signature (status: $($signature.Status))."
        }
    }
    return $item
}

function Assert-ForkFeeder([string]$Path) {
    $item = Assert-X64Pe $Path 'DLSS feeder add-on'
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($item.FullName))
    foreach ($marker in @('standalone DLSS neural reconstruction', 'L - DLSS 4.5 (Ultra Performance tuned)')) {
        if ($text.IndexOf($marker, [StringComparison]::Ordinal) -lt 0) {
            if ($AllowUpstreamFeederForTesting) {
                Write-Warning "Using the upstream feeder binary for a private test; CK3 fork overlay labels are unavailable."
                return $item.FullName
            }
        }
    }
    return $item.FullName
}

function Copy-DirectoryContents([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Container)) { throw "Directory was not found: '$Source'" }
    Ensure-Directory $Destination
    Get-ChildItem -LiteralPath $Source -Force | Copy-Item -Destination $Destination -Recurse -Force
}

function Get-FeedLayerArchive([string]$LocalPath, [string]$Destination) {
    if ($LocalPath) {
        return [pscustomobject]@{ Path = (Assert-File $LocalPath 'Feeder Vulkan layer archive' 100); Source = 'Maintainer-supplied local archive' }
    }
    Write-Step 'Downloading the feeder Vulkan layer from the upstream project release...'
    Invoke-WebRequest -Uri $script:FeedLayerUrl -OutFile $Destination -UseBasicParsing -Headers @{ 'User-Agent' = 'CK3-DLSS-Package-Builder' }
    return [pscustomobject]@{ Path = (Assert-File $Destination 'Feeder Vulkan layer archive' 100); Source = $script:FeedLayerUrl }
}

function Get-VersionFamily([string]$Value) {
    $match = [regex]::Match($Value, '^(\d+\.\d+)')
    if ($match.Success) { return $match.Groups[1].Value }
    return ''
}

function Add-RuntimeFile(
    [string]$Source,
    [string]$Destination,
    [string]$Label,
    [bool]$ExpectNvidia,
    [bool]$AllowModified,
    [System.Collections.Generic.List[string]]$Provenance
) {
    if (-not $Source) { return $null }
    $item = if ($ExpectNvidia) { Assert-NvidiaPe $Source $Label $AllowModified } else { Assert-X64Pe $Source $Label }
    Ensure-Directory (Split-Path -Parent $Destination)
    Copy-Item -LiteralPath $item.FullName -Destination $Destination -Force
    $Provenance.Add("$Label.Source=maintainer-supplied:$($item.Name)")
    $Provenance.Add("$Label.Sha256=$(Get-Sha256 $Destination)")
    $Provenance.Add("$Label.FileVersion=$($item.VersionInfo.FileVersion)")
    return $item
}

function Add-RuntimePair(
    [string]$Profile,
    [string]$BaseSource,
    [string]$NrSource,
    [bool]$AllowModifiedNr,
    [string]$RuntimeRoot,
    [System.Collections.Generic.List[string]]$Provenance
) {
    if (-not $BaseSource -and -not $NrSource) { return }
    if (-not $BaseSource -or -not $NrSource) { throw "$Profile offline staging requires both its nvngx_dlss.dll and nvngx_dlssnr.dll." }
    $profileRoot = Join-Path $RuntimeRoot $Profile
    $base = Add-RuntimeFile $BaseSource (Join-Path $profileRoot 'nvngx_dlss.dll') "$Profile.DLSS" $true $false $Provenance
    $nr = Add-RuntimeFile $NrSource (Join-Path $profileRoot 'nvngx_dlssnr.dll') "$Profile.DLSS-NR" $true $AllowModifiedNr $Provenance
    $baseFamily = Get-VersionFamily ([string]$base.VersionInfo.FileVersion)
    $nrFamily = Get-VersionFamily ([string]$nr.VersionInfo.FileVersion)
    if ($baseFamily -and $nrFamily -and $baseFamily -ne $nrFamily) {
        throw "$Profile runtimes are from different version families: $($base.VersionInfo.FileVersion) and $($nr.VersionInfo.FileVersion)."
    }
}

$template = Join-Path $PSScriptRoot 'ck3-package'
if (-not (Test-Path -LiteralPath $template -PathType Container)) { throw "CK3 package template not found: '$template'" }
$feeder = Assert-ForkFeeder $FeederAddon

$output = [IO.Path]::GetFullPath($OutputDirectory)
$releaseRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'release'))
if (-not $output.StartsWith($releaseRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputDirectory must remain below '$releaseRoot'."
}

$hasOptionalRuntimes = $Dlss45Runtime -or $Dlss5Runtime -or $Dlss5NrRuntime -or
    $Dlss5ExtendedRuntime -or $Dlss5ExtendedNrRuntime -or $RenoDxAddon
if ($hasOptionalRuntimes -and -not $AcknowledgeRuntimeRedistributionTerms) {
    throw 'Optional NVIDIA/RenoDX files are for a private offline build only. Pass -AcknowledgeRuntimeRedistributionTerms after reviewing every supplied file license; do not publish the result without permission.'
}

$temp = Join-Path ([IO.Path]::GetTempPath()) ('ck3-dlss-build-' + [guid]::NewGuid().ToString('N'))
Ensure-Directory $temp
try {
    $layer = Get-FeedLayerArchive $FeedLayerZip (Join-Path $temp 'feed-vk-layer.zip')
    $layerExtract = Join-Path $temp 'layer'
    Expand-Archive -LiteralPath $layer.Path -DestinationPath $layerExtract -Force

    if (Test-Path -LiteralPath $output -PathType Container) {
        Write-Step "Replacing old staging directory '$output'."
        Remove-Item -LiteralPath $output -Recurse -Force
    }
    Ensure-Directory $output
    Copy-DirectoryContents $template $output

    $binaryRoot = Join-Path $output 'binaries'
    $payloadRoot = Join-Path $binaryRoot 'dlss-payload'
    $runtimeRoot = Join-Path $payloadRoot 'runtimes'
    $layerRoot = Join-Path $binaryRoot 'dlss5-vulkan'
    $shaderRoot = Join-Path $binaryRoot 'reshade-shaders\\Shaders'
    Ensure-Directory $payloadRoot
    Ensure-Directory $runtimeRoot
    Ensure-Directory $layerRoot
    Ensure-Directory $shaderRoot

    Copy-Item -LiteralPath $feeder -Destination (Join-Path $payloadRoot 'dlss5-feed.addon64') -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'shaders\\DLSS5_Feed.fx') -Destination $shaderRoot -Force

    foreach ($name in @('VkLayer_feed_vk.dll', 'VkLayer_feed_vk.json')) {
        $matches = @(Get-ChildItem -LiteralPath $layerExtract -Recurse -File -Filter $name)
        if ($matches.Count -ne 1) { throw "Expected one $name in the feeder Vulkan layer archive; found $($matches.Count)." }
        Copy-Item -LiteralPath $matches[0].FullName -Destination $layerRoot -Force
    }
    Assert-X64Pe (Join-Path $layerRoot 'VkLayer_feed_vk.dll') 'Feeder Vulkan layer' | Out-Null
    $layerManifestPath = Join-Path $layerRoot 'VkLayer_feed_vk.json'
    $layerManifestText = Get-Content -LiteralPath $layerManifestPath -Raw
    $layerManifestText = $layerManifestText.Replace('".\VkLayer_feed_vk.dll"', '".\\VkLayer_feed_vk.dll"')
    [IO.File]::WriteAllText($layerManifestPath, $layerManifestText, [Text.UTF8Encoding]::new($false))
    try { $layerJson = Get-Content -LiteralPath $layerManifestPath -Raw | ConvertFrom-Json }
    catch { throw "The normalized feeder Vulkan manifest is invalid: $($_.Exception.Message)" }
    if ([string]$layerJson.layer.name -ne 'VK_LAYER_feed_vk') { throw 'The feeder Vulkan manifest has the wrong layer name.' }

    $licenseRoot = Join-Path $output 'THIRD-PARTY-LICENSES'
    Ensure-Directory $licenseRoot
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'LICENSE') -Destination (Join-Path $licenseRoot 'DLSS5-Feeder-LICENSE.txt') -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'external\\minhook\\LICENSE.txt') -Destination (Join-Path $licenseRoot 'MinHook-LICENSE.txt') -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'external\\imgui\\LICENSE.txt') -Destination (Join-Path $licenseRoot 'Dear-ImGui-LICENSE.txt') -Force

    $provenance = [System.Collections.Generic.List[string]]::new()
    $provenance.Add('CK3 DLSS portable bootstrap build provenance')
    $provenance.Add("BuiltUtc=$([DateTime]::UtcNow.ToString('o'))")
    $provenance.Add("FeederSource=maintainer-supplied CK3 fork build:$([IO.Path]::GetFileName($feeder))")
    $provenance.Add("FeederSha256=$(Get-Sha256 (Join-Path $payloadRoot 'dlss5-feed.addon64'))")
    $provenance.Add("FeedLayerSource=$($layer.Source)")
    $provenance.Add("FeedLayerArchiveSha256=$(Get-Sha256 $layer.Path)")
    $provenance.Add('ReShade=not bundled; pinned official setup is downloaded by the end-user bootstrap')
    $provenance.Add('ReShade.fxh=not bundled; pinned official source is downloaded by the end-user bootstrap')
    $provenance.Add('VORT=not bundled; pinned official source is downloaded by the end-user bootstrap')

    if ($Dlss45Runtime) {
        Add-RuntimeFile $Dlss45Runtime (Join-Path $runtimeRoot 'DLSS45\\nvngx_dlss.dll') 'DLSS45.DLSS' $true $false $provenance | Out-Null
    }
    Add-RuntimePair 'DLSS5' $Dlss5Runtime $Dlss5NrRuntime $false $runtimeRoot $provenance
    Add-RuntimePair 'DLSS5Extended' $Dlss5ExtendedRuntime $Dlss5ExtendedNrRuntime $true $runtimeRoot $provenance
    if ($RenoDxAddon) {
        Add-RuntimeFile $RenoDxAddon (Join-Path $runtimeRoot 'shared\\renodx-dlss5.addon64') 'RenoDX.DLSS5' $false $false $provenance | Out-Null
    }
    if (-not $hasOptionalRuntimes) {
        $provenance.Add('OptionalRuntimeFiles=none; acquired on the end-user machine after explicit consent')
    }

    Remove-Item -LiteralPath (Join-Path $layerRoot 'README-PAYLOAD.txt') -Force -ErrorAction SilentlyContinue
    [IO.File]::WriteAllLines((Join-Path $output 'BUILD-PROVENANCE.txt'), $provenance, [Text.UTF8Encoding]::new($false))

    $zipPath = "$output.zip"
    if (Test-Path -LiteralPath $zipPath -PathType Leaf) { Remove-Item -LiteralPath $zipPath -Force }
    Compress-Archive -Path (Join-Path $output '*') -DestinationPath $zipPath -CompressionLevel Optimal
    Write-Step "Created '$zipPath'."
    if ($hasOptionalRuntimes) {
        Write-Warning 'This private/offline ZIP contains supplied runtime binaries. Do not publish it without all required permissions and notices.'
    }
    else {
        Write-Step 'Created a public bootstrap: third-party graphics and proprietary runtime files are acquired on the end-user machine.'
    }

    if (-not $KeepStagingDirectory) {
        Remove-Item -LiteralPath $output -Recurse -Force
        Write-Step 'Removed the unpacked staging directory (use -KeepStagingDirectory to retain it).'
    }
}
finally {
    if (Test-Path -LiteralPath $temp -PathType Container) { Remove-Item -LiteralPath $temp -Recurse -Force }
}

