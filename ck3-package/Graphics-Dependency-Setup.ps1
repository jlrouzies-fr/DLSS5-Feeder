[CmdletBinding()]
param(
    [ValidateSet('Install', 'Status')]
    [string]$Action = 'Install',

    [string]$GameRoot = $PSScriptRoot,
    [string]$CacheRoot,
    [string]$ReShadeSetup,
    [string]$ReShadeFfx,
    [string]$VortShadersZip,
    [switch]$AcceptDependencyLicenses,
    [switch]$ForceDownload
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

# These immutable inputs were tested together. Updating one requires updating its hash and tests.
$script:ReShadeVersion = '6.8.0'
$script:ReShadeUrl = 'https://reshade.me/downloads/ReShade_Setup_6.8.0_Addon.exe'
$script:ReShadeSetupSha256 = 'afe4c8f13048306307983b8b3d41d5bf00a86820440b0e57dea10950e1176445'
$script:ReShadeFfxCommit = '6db142b4b1a05c764222e5b0bd9a644b7ccfe1dc'
$script:ReShadeFfxUrl = "https://raw.githubusercontent.com/crosire/reshade-shaders/$($script:ReShadeFfxCommit)/Shaders/ReShade.fxh"
$script:ReShadeFfxSha256 = '6dabfbbaf968c3871905d2ea17f96572ff7b1cec01310b5d0e5252b66b30174f'
$script:VortCommit = 'b410b9f0c0fbb83c8cb42164aaf1655fab386f4a'
$script:VortUrl = "https://codeload.github.com/vortigern11/vort_Shaders/zip/$($script:VortCommit)"
$script:VortArchiveSha256 = '231ba34a75556f9943e359559a89b0d0cc2caa322d9dcdee5630061bf9fe13b6'
$script:LiliumVersion = '2026.02.28'
$script:LiliumSource = 'https://github.com/EndlesslyFlowering/ReShade_HDR_shaders/releases/tag/2026.02.28'
$script:LiliumArchiveSha256 = '3dc9f9dd70c9ae7dfbb3d770032afbc1998d46aece4ee247460462ed53815488'


function Write-GraphicsStatus([string]$Message) {
    Write-Host "[CK3 DLSS graphics] $Message"
}

function Resolve-PackageRoot([string]$Candidate) {
    $resolved = [IO.Path]::GetFullPath($Candidate.TrimEnd([char[]]@(92, 47)))
    if (Test-Path -LiteralPath (Join-Path $resolved 'binaries\\ck3.exe') -PathType Leaf) { return $resolved }
    if ((Split-Path -Leaf $resolved) -ieq 'binaries' -and
        (Test-Path -LiteralPath (Join-Path $resolved 'ck3.exe') -PathType Leaf)) {
        return (Split-Path -Parent $resolved)
    }
    throw "binaries\\ck3.exe was not found below '$resolved'. Extract the package into the Crusader Kings III root folder."
}

function Ensure-Directory([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-Hash([string]$Path, [string]$Expected, [string]$Label) {
    $actual = Get-Sha256 $Path
    if ($actual -ne $Expected.ToLowerInvariant()) {
        throw "$Label failed SHA-256 verification. Expected $Expected, received $actual. Delete the cached file and retry."
    }
    return $actual
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
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Label is missing: '$Path'" }
    if ((Get-Item -LiteralPath $Path).Length -lt 4096) { throw "$Label is unexpectedly small: '$Path'" }
    $machine = Get-PeMachine $Path
    if ($machine -ne 0x8664) { throw "$Label is not x64 (machine 0x$($machine.ToString('X4')))." }
    return (Get-Item -LiteralPath $Path)
}

function Save-VerifiedDownload([string]$Uri, [string]$Destination, [string]$ExpectedHash, [string]$Label) {
    Ensure-Directory (Split-Path -Parent $Destination)
    if ($ForceDownload -or -not (Test-Path -LiteralPath $Destination -PathType Leaf)) {
        $partial = "$Destination.partial"
        if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
        Write-GraphicsStatus "Downloading $Label..."
        try {
            Invoke-WebRequest -Uri $Uri -OutFile $partial -UseBasicParsing -Headers @{ 'User-Agent' = 'CK3-DLSS-Bootstrap' }
            Assert-Hash $partial $ExpectedHash $Label | Out-Null
            Move-Item -LiteralPath $partial -Destination $Destination -Force
        }
        finally {
            if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
        }
    }
    Assert-Hash $Destination $ExpectedHash $Label | Out-Null
    return (Get-Item -LiteralPath $Destination).FullName
}

function Resolve-Input(
    [string]$ExplicitPath,
    [string]$CachePath,
    [string]$Uri,
    [string]$ExpectedHash,
    [string]$Label
) {
    if ($ExplicitPath) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) { throw "$Label was not found: '$ExplicitPath'" }
        return [pscustomobject]@{
            Path = (Get-Item -LiteralPath $ExplicitPath).FullName
            Source = 'User-supplied local file'
            Hash = Get-Sha256 $ExplicitPath
        }
    }
    $path = Save-VerifiedDownload $Uri $CachePath $ExpectedHash $Label
    return [pscustomobject]@{ Path = $path; Source = $Uri; Hash = $ExpectedHash }
}

function Expand-ReShadeSetup([string]$SetupPath, [string]$Destination) {
    $data = [IO.File]::ReadAllBytes($SetupPath)
    $archiveOffset = -1
    for ($offset = 0; $offset -le $data.Length - 4; $offset += 512) {
        if ($data[$offset] -eq 0x50 -and $data[$offset + 1] -eq 0x4B -and
            $data[$offset + 2] -eq 0x03 -and $data[$offset + 3] -eq 0x04) {
            $archiveOffset = $offset
            break
        }
    }
    if ($archiveOffset -lt 0) { throw "Could not locate ReShade's embedded archive in '$SetupPath'." }

    $memory = [IO.MemoryStream]::new()
    try {
        $memory.Write($data, $archiveOffset, $data.Length - $archiveOffset)
        $memory.Position = 0
        $archive = [IO.Compression.ZipArchive]::new($memory, [IO.Compression.ZipArchiveMode]::Read, $true)
        try {
            $matches = @($archive.Entries | Where-Object { $_.FullName -eq 'ReShade64.dll' })
            if ($matches.Count -ne 1) { throw "Expected one ReShade64.dll in the setup; found $($matches.Count)." }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($matches[0], $Destination, $true)
        }
        finally { $archive.Dispose() }
    }
    finally { $memory.Dispose() }
}

function Expand-SafeZip([string]$ArchivePath, [string]$Destination) {
    Ensure-Directory $Destination
    $destinationFull = [IO.Path]::GetFullPath($Destination).TrimEnd([char[]]@(92, 47)) + [IO.Path]::DirectorySeparatorChar
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        foreach ($entry in $archive.Entries) {
            $relative = $entry.FullName.Replace('/', '\\')
            if (-not $relative) { continue }
            if ([IO.Path]::IsPathRooted($relative) -or $relative.Split('\\') -contains '..') {
                throw "Archive contains an unsafe path: '$($entry.FullName)'"
            }
            $target = [IO.Path]::GetFullPath((Join-Path $Destination $relative))
            if (-not $target.StartsWith($destinationFull, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Archive entry escapes the staging directory: '$($entry.FullName)'"
            }
            if (-not $entry.Name) {
                Ensure-Directory $target
                continue
            }
            Ensure-Directory (Split-Path -Parent $target)
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
        }
    }
    finally { $archive.Dispose() }
}

function Get-VortRoot([string]$ExtractRoot) {
    $candidates = @(Get-ChildItem -LiteralPath $ExtractRoot -Directory | Where-Object {
        (Test-Path -LiteralPath (Join-Path $_.FullName 'Shaders\\vort_Motion.fx') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'Shaders\\Includes\\vort_MotionUtils.fxh') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'LICENSE') -PathType Leaf)
    })
    if ($candidates.Count -ne 1) { throw "Expected one valid VORT shader root; found $($candidates.Count)." }
    return $candidates[0].FullName
}

function Assert-CK3Closed {
    $running = @(Get-Process -Name 'ck3' -ErrorAction SilentlyContinue)
    if ($running.Count) { throw 'Crusader Kings III is running. Close the game before installing or changing DLSS files.' }
}

function Test-GraphicsDependencies([string]$BinaryRoot) {
    $required = @(
        'dlss5-vulkan\\ReShade64.dll',
        'dlss5-vulkan\\ReShade64.json',
        'reshade-shaders\\Shaders\\DLSS5_Feed.fx',
        'reshade-shaders\\Shaders\\ReShade.fxh',
        'third-party\\vort_Shaders\\Shaders\\vort_Motion.fx',
        'third-party\\vort_Shaders\\Shaders\\Includes\\vort_MotionUtils.fxh',
        'third-party\\vort_Shaders\\LICENSE',
        'reshade-shaders\Shaders\Lilium\lilium__tone_mapping.fx',
        'reshade-shaders\Shaders\Lilium\lilium__include\include_main.fxh',
        'reshade-shaders\Textures\Lilium\lilium__blue_noise_64x64.png',
        'CK3-DLSS-GRAPHICS.json'
    )
    $missing = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $BinaryRoot $_) -PathType Leaf) })
    if ($missing.Count) { throw "Graphics dependencies are missing: $($missing -join ', ')" }
    Assert-X64Pe (Join-Path $BinaryRoot 'dlss5-vulkan\\ReShade64.dll') 'ReShade64.dll' | Out-Null

    $liliumLicense = Join-Path (Split-Path -Parent $BinaryRoot) 'THIRD-PARTY-LICENSES\Lilium-GPL-3.0.txt'
    if (-not (Test-Path -LiteralPath $liliumLicense -PathType Leaf)) { throw 'The bundled Lilium GPL-3.0 license is missing.' }
    $liliumShaderRoot = Join-Path $BinaryRoot 'reshade-shaders\Shaders\Lilium'
    $liliumTextureRoot = Join-Path $BinaryRoot 'reshade-shaders\Textures\Lilium'
    $liliumShaderCount = @(Get-ChildItem -LiteralPath $liliumShaderRoot -Recurse -File).Count
    $liliumTextureCount = @(Get-ChildItem -LiteralPath $liliumTextureRoot -Recurse -File).Count
    if ($liliumShaderCount -ne 44 -or $liliumTextureCount -ne 3) {
        throw "The bundled Lilium payload is incomplete: $liliumShaderCount shader files, $liliumTextureCount textures."
    }

    try { $state = Get-Content -LiteralPath (Join-Path $BinaryRoot 'CK3-DLSS-GRAPHICS.json') -Raw | ConvertFrom-Json }
    catch { throw "The graphics dependency receipt is invalid: $($_.Exception.Message)" }
    if ([string]$state.SchemaVersion -ne '1') { throw 'The graphics dependency receipt has an unsupported schema.' }
    if ((Get-Sha256 (Join-Path $BinaryRoot 'dlss5-vulkan\\ReShade64.dll')) -ne [string]$state.Components.ReShade.DllSha256) {
        throw 'ReShade64.dll no longer matches the installed dependency receipt.'
    }
    if ([string]$state.Components.Lilium.Version -ne $script:LiliumVersion) { throw 'The graphics dependency receipt does not describe the bundled Lilium release.' }
    if ((Get-Sha256 (Join-Path $BinaryRoot 'reshade-shaders\\Shaders\\ReShade.fxh')) -ne [string]$state.Components.ReShadeFfx.Sha256) {
        throw 'ReShade.fxh no longer matches the installed dependency receipt.'
    }
    Write-GraphicsStatus "Graphics dependencies validated (ReShade $($state.Components.ReShade.Version), VORT $($state.Components.Vort.Commit.Substring(0, 8)), Lilium $($state.Components.Lilium.Version))."
    return $state
}

function Confirm-DependencyInstall {
    if ($AcceptDependencyLicenses) { return }
    Write-Host ''
    Write-Host 'First-run graphics dependency download' -ForegroundColor Yellow
    Write-Host '  ReShade full add-on 6.8.0 will be downloaded from reshade.me and extracted locally.'
    Write-Host '  ReShade.fxh (CC0) and VORT motion vectors (MIT) will be downloaded from their official repositories.'
    Write-Host '  ReShade, ReShade.fxh, and VORT are downloaded separately; Lilium 2026.02.28 is bundled under GPL-3.0.'
    Write-Host ''
    $answer = (Read-Host 'Type DOWNLOAD to continue').Trim()
    if ($answer -cne 'DOWNLOAD') { throw 'Graphics dependency setup was cancelled; no renderer setting was changed.' }
}

function Install-GraphicsDependencies([string]$BinaryRoot, [string]$DownloadRoot) {
    if (-not $ForceDownload) {
        try { return Test-GraphicsDependencies $BinaryRoot }
        catch { Write-GraphicsStatus 'A complete validated graphics installation was not found; assembling it now.' }
    }

    Assert-CK3Closed
    Confirm-DependencyInstall
    Ensure-Directory $DownloadRoot
    $setup = Resolve-Input $ReShadeSetup (Join-Path $DownloadRoot "ReShade_Setup_$($script:ReShadeVersion)_Addon.exe") `
        $script:ReShadeUrl $script:ReShadeSetupSha256 'ReShade full add-on setup'
    $ffx = Resolve-Input $ReShadeFfx (Join-Path $DownloadRoot "ReShade-$($script:ReShadeFfxCommit).fxh") `
        $script:ReShadeFfxUrl $script:ReShadeFfxSha256 'ReShade.fxh'
    $vort = Resolve-Input $VortShadersZip (Join-Path $DownloadRoot "vort_Shaders-$($script:VortCommit).zip") `
        $script:VortUrl $script:VortArchiveSha256 'VORT motion-vector shaders'

    $stage = Join-Path $BinaryRoot ('.ck3-dlss-graphics-stage-' + [guid]::NewGuid().ToString('N'))
    Ensure-Directory $stage
    try {
        $stageReShade = Join-Path $stage 'ReShade64.dll'
        Expand-ReShadeSetup $setup.Path $stageReShade
        $reShadeInfo = Assert-X64Pe $stageReShade 'Extracted ReShade64.dll'

        $stageFfx = Join-Path $stage 'ReShade.fxh'
        Copy-Item -LiteralPath $ffx.Path -Destination $stageFfx -Force
        if ($ffx.Source -ne 'User-supplied local file') { Assert-Hash $stageFfx $script:ReShadeFfxSha256 'ReShade.fxh' | Out-Null }

        $vortExtract = Join-Path $stage 'vort-extract'
        Expand-SafeZip $vort.Path $vortExtract
        $stageVort = Get-VortRoot $vortExtract

        $layerRoot = Join-Path $BinaryRoot 'dlss5-vulkan'
        $shaderRoot = Join-Path $BinaryRoot 'reshade-shaders\\Shaders'
        $thirdPartyRoot = Join-Path $BinaryRoot 'third-party'
        Ensure-Directory $layerRoot
        Ensure-Directory $shaderRoot
        Ensure-Directory $thirdPartyRoot

        Copy-Item -LiteralPath $stageReShade -Destination (Join-Path $layerRoot 'ReShade64.dll') -Force
        Copy-Item -LiteralPath $stageFfx -Destination (Join-Path $shaderRoot 'ReShade.fxh') -Force
        $vortDestination = Join-Path $thirdPartyRoot 'vort_Shaders'
        if (Test-Path -LiteralPath $vortDestination -PathType Container) {
            Remove-Item -LiteralPath $vortDestination -Recurse -Force
        }
        Copy-Item -LiteralPath $stageVort -Destination $vortDestination -Recurse -Force

        $state = [ordered]@{
            SchemaVersion = 1
            InstalledUtc = [DateTime]::UtcNow.ToString('o')
            Components = [ordered]@{
                ReShade = [ordered]@{
                    Version = $script:ReShadeVersion
                    Source = $setup.Source
                    SetupSha256 = $setup.Hash
                    DllSha256 = Get-Sha256 (Join-Path $layerRoot 'ReShade64.dll')
                    FileVersion = $reShadeInfo.VersionInfo.FileVersion
                    SignatureNote = 'The official full add-on build is intentionally unsigned.'
                }
                ReShadeFfx = [ordered]@{
                    Commit = $script:ReShadeFfxCommit
                    Source = $ffx.Source
                    Sha256 = Get-Sha256 (Join-Path $shaderRoot 'ReShade.fxh')
                    License = 'CC0-1.0 (SPDX header retained in the file)'
                }
                Vort = [ordered]@{
                    Commit = $script:VortCommit
                    Source = $vort.Source
                    ArchiveSha256 = $vort.Hash
                    License = 'MIT (LICENSE retained beside the installed shaders)'
                }
                Lilium = [ordered]@{
                    Version = $script:LiliumVersion
                    Source = $script:LiliumSource
                    ArchiveSha256 = $script:LiliumArchiveSha256
                    ShaderFiles = 44
                    TextureFiles = 3
                    License = 'GPL-3.0 (complete shader source and license bundled)'
                }
            }
        }
        $json = $state | ConvertTo-Json -Depth 6
        [IO.File]::WriteAllText((Join-Path $BinaryRoot 'CK3-DLSS-GRAPHICS.json'), $json + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
        return Test-GraphicsDependencies $BinaryRoot
    }
    finally {
        if (Test-Path -LiteralPath $stage -PathType Container) { Remove-Item -LiteralPath $stage -Recurse -Force }
    }
}

$root = Resolve-PackageRoot $GameRoot
$binaryRoot = Join-Path $root 'binaries'
if (-not $CacheRoot) {
    $localData = [Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)
    $CacheRoot = Join-Path $localData 'CK3-DLSS\\cache\\graphics'
}
$CacheRoot = [IO.Path]::GetFullPath($CacheRoot)

switch ($Action) {
    'Install' { Install-GraphicsDependencies $binaryRoot $CacheRoot | Out-Null }
    'Status' { Test-GraphicsDependencies $binaryRoot | Out-Null }
}

