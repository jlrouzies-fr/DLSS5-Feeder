[CmdletBinding()]
param(
    [ValidateSet('Configure', 'Status', 'OpenRHI')]
    [string]$Action = 'Configure',

    [ValidateSet('Auto', 'DLSS45', 'DLSS5', 'DLSS5Extended')]
    [string]$Mode = 'Auto',

    [string]$GameRoot = $PSScriptRoot,
    [string]$DlssRuntime,
    [string]$DlssNrRuntime,
    [string]$RenoDxAddon,
    [switch]$AcceptRuntimeLicenses,
    [switch]$AllowUnsignedNvidiaRuntime,
    [switch]$ForceDownload
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$script:Headers = @{ 'User-Agent' = 'CK3-DLSS-Portable' }
$script:NvidiaLatestRelease = 'https://api.github.com/repos/NVIDIA/DLSS/releases/latest'
$script:NvidiaLicense = 'https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt'
$script:RhiManifest = 'https://raw.githubusercontent.com/RankFTW/RHI/main/dlss_manifest.json'
$script:RhiReleases = 'https://api.github.com/repos/RankFTW/RHI/releases/latest'
$script:RhiRepoReleases = 'https://api.github.com/repos/RankFTW/rhi-repo/releases?per_page=100'
# RHI's manifest has no compatibility or hash fields. These reviewed pairs fail closed if
# the manifest changes, especially for the unsigned ShortFuse variants.
$script:RhiPairPolicy = @{
    '310.8.0' = [ordered]@{
        BaseVersion = '310.8.0'
        BaseArchiveSha256 = 'fb481660f7e952b87f91760e3afd7f9dc14cd2c3361b470e948d6346e4323009'
        BaseDllSha256 = 'c85f971ce023c9f3492fc7455f0b01a24ba18ea39636407a846902c4360b0b7e'
        NrArchiveSha256 = '388c0a7912e15ec911b9c9e11a692142b11fe387ddf2b637d8c358138fffb3ac'
        NrDllSha256 = 'e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e'
        Modified = $false
    }
    '310.8.SF' = [ordered]@{
        BaseVersion = '310.8.0'
        BaseArchiveSha256 = 'fb481660f7e952b87f91760e3afd7f9dc14cd2c3361b470e948d6346e4323009'
        BaseDllSha256 = 'c85f971ce023c9f3492fc7455f0b01a24ba18ea39636407a846902c4360b0b7e'
        NrArchiveSha256 = '4de991bf3a1cf5ba95c6621c2a5203299e823ea11bf1702513281b85aa4dc449'
        NrDllSha256 = '4c5bd1171c7336b4b04fb394de51da285ab6ead6f922d7afdec163f71c319d74'
        Modified = $true
    }
    '310.8.SF-v2' = [ordered]@{
        BaseVersion = '310.8.0'
        BaseArchiveSha256 = 'fb481660f7e952b87f91760e3afd7f9dc14cd2c3361b470e948d6346e4323009'
        BaseDllSha256 = 'c85f971ce023c9f3492fc7455f0b01a24ba18ea39636407a846902c4360b0b7e'
        NrArchiveSha256 = '1da35941894994eb087e017577829e492454e9bae3a6a9397027069ceb74955c'
        NrDllSha256 = '6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927'
        Modified = $true
    }
}
$script:RhiProject = 'https://github.com/RankFTW/RHI'

try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
}
catch { }

function Write-RuntimeStatus([string]$Message) {
    Write-Host "[CK3 DLSS runtime] $Message"
}

function Resolve-PackageRoot([string]$Candidate) {
    $resolved = [IO.Path]::GetFullPath($Candidate.TrimEnd([char[]]@(92, 47)))
    if ((Split-Path -Leaf $resolved) -ieq 'binaries') {
        $resolved = Split-Path -Parent $resolved
    }
    if (-not (Test-Path -LiteralPath (Join-Path $resolved 'binaries') -PathType Container)) {
        throw "The package binaries folder was not found below '$resolved'."
    }
    return $resolved
}

function Ensure-Directory([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-PeMachine([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 4096 -or $reader.ReadUInt16() -ne 0x5A4D) {
            throw "'$Path' is not a valid Windows PE file."
        }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 0x40 -or ($peOffset + 6) -gt $stream.Length) {
            throw "'$Path' has an invalid PE header offset."
        }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "'$Path' does not contain a valid PE signature."
        }
        return $reader.ReadUInt16()
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Assert-PeFile(
    [string]$Path,
    [string]$Label,
    [bool]$RequireX64 = $true,
    [bool]$ExpectNvidiaSignature = $false,
    [bool]$AllowModified = $false
) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label was not found: '$Path'"
    }
    $resolved = (Get-Item -LiteralPath $Path).FullName
    $machine = Get-PeMachine $resolved
    if ($RequireX64 -and $machine -ne 0x8664) {
        throw "$Label is not an x64 PE file (machine 0x$($machine.ToString('X4'))): '$resolved'"
    }

    $signatureStatus = 'Unavailable'
    $signer = ''
    try {
        $signature = Get-AuthenticodeSignature -LiteralPath $resolved
        $signatureStatus = [string]$signature.Status
        if ($signature.SignerCertificate) { $signer = $signature.SignerCertificate.Subject }
    }
    catch { }

    if ($ExpectNvidiaSignature -and -not $AllowModified) {
        $isNvidia = $signatureStatus -eq 'Valid' -and $signer -match '(?i)NVIDIA'
        if (-not $isNvidia -and -not $AllowUnsignedNvidiaRuntime) {
            throw "$Label did not have a valid NVIDIA Authenticode signature (status: $signatureStatus). Use only a trusted NVIDIA runtime, or pass -AllowUnsignedNvidiaRuntime for an audited local test file."
        }
        if (-not $isNvidia) {
            Write-Warning "$Label is being accepted without a valid NVIDIA signature because -AllowUnsignedNvidiaRuntime was specified."
        }
    }
    elseif ($AllowModified -and $signatureStatus -ne 'Valid') {
        Write-Warning "$Label is a modified compatibility runtime; its NVIDIA signature is expected to be invalid."
    }

    $item = Get-Item -LiteralPath $resolved
    return [pscustomobject]@{
        Path = $resolved
        Sha256 = Get-Sha256 $resolved
        FileVersion = $item.VersionInfo.FileVersion
        SignatureStatus = $signatureStatus
        Signer = $signer
        Machine = 'x64'
    }
}

function Get-RemoteJson([string]$Uri, [string]$Label) {
    Write-RuntimeStatus "Checking $Label..."
    try {
        return Invoke-RestMethod -Uri $Uri -Headers $script:Headers -UseBasicParsing
    }
    catch {
        throw "Could not read $Label from '$Uri': $($_.Exception.Message)"
    }
}

function Save-Download([string]$Uri, [string]$Destination, [string]$Label) {
    Ensure-Directory (Split-Path -Parent $Destination)
    $partial = "$Destination.partial"
    if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
    Write-RuntimeStatus "Downloading $Label..."
    try {
        Invoke-WebRequest -Uri $Uri -Headers $script:Headers -OutFile $partial -UseBasicParsing
        Move-Item -LiteralPath $partial -Destination $Destination -Force
    }
    finally {
        if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
    }
}

function Copy-ArchivePayload([string]$Archive, [string]$ExpectedName, [string]$Destination, [string]$Label) {
    $temp = Join-Path ([IO.Path]::GetTempPath()) ('ck3-dlss-extract-' + [guid]::NewGuid().ToString('N'))
    Ensure-Directory $temp
    try {
        Expand-Archive -LiteralPath $Archive -DestinationPath $temp -Force
        $matches = @(Get-ChildItem -LiteralPath $temp -Recurse -File | Where-Object { $_.Name -ieq $ExpectedName })
        if ($matches.Count -ne 1) {
            throw "$Label archive contained $($matches.Count) files named '$ExpectedName'; expected exactly one."
        }
        Ensure-Directory (Split-Path -Parent $Destination)
        Copy-Item -LiteralPath $matches[0].FullName -Destination $Destination -Force
    }
    finally {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
    }
}

function Get-GpuNames {
    try {
        return @(
            Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop |
                ForEach-Object { $_.Name } |
                Where-Object { $_ }
        )
    }
    catch {
        return @()
    }
}

function Select-RuntimeMode([string[]]$GpuNames) {
    $gpuText = if ($GpuNames.Count) { $GpuNames -join '; ' } else { 'GPU detection unavailable' }
    Write-Host ''
    Write-Host 'Choose the CK3 DLSS profile:' -ForegroundColor Cyan
    Write-Host "  Detected: $gpuText"
    Write-Host ''
    Write-Host '  1. DLSS 4.5 Neural Reconstruction (Model M DLAA; recommended for RTX 20/30/40)'
    Write-Host '  2. DLSS 5 Neural Rendering       (NVIDIA-signed stock NR preview via RHI; support not promised)'
    Write-Host '  3. DLSS 5 Extended               (experimental RHI/ShortFuse runtime for RTX 20/30/40)'
    Write-Host '  Q. Cancel'
    Write-Host ''
    $selection = (Read-Host 'Selection [1]').Trim()
    if (-not $selection) { $selection = '1' }
    switch ($selection.ToUpperInvariant()) {
        '1' { return 'DLSS45' }
        '2' { return 'DLSS5' }
        '3' { return 'DLSS5Extended' }
        'Q' { throw 'Runtime setup was cancelled.' }
        default { throw "Unknown selection '$selection'. Run setup again and choose 1, 2, 3, or Q." }
    }
}

function Confirm-Downloads([string]$SelectedMode) {
    if ($AcceptRuntimeLicenses) { return }
    Write-Host ''
    Write-Host 'Runtime download notice' -ForegroundColor Yellow
    Write-Host '  The package will download NVIDIA DLSS from NVIDIA''s official GitHub release.'
    if ($SelectedMode -ne 'DLSS45') {
        Write-Host '  It will also use RHI''s public manifest/repository for RenoDX and DLSS NR.'
    }
    if ($SelectedMode -eq 'DLSS5Extended') {
        Write-Host '  The Extended profile uses a third-party modified ShortFuse DLSS NR runtime.' -ForegroundColor Yellow
    }
    Write-Host "  NVIDIA terms: $($script:NvidiaLicense)"
    Write-Host "  RHI project:   $($script:RhiProject)"
    Write-Host ''
    $answer = (Read-Host 'Type DOWNLOAD to continue').Trim()
    if ($answer -cne 'DOWNLOAD') { throw 'No runtime files were downloaded.' }
}

function Find-LocalFile([string]$ExplicitPath, [string[]]$Candidates, [string]$Label) {
    if ($ExplicitPath) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "$Label was not found: '$ExplicitPath'"
        }
        return (Get-Item -LiteralPath $ExplicitPath).FullName
    }
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Get-Item -LiteralPath $candidate).FullName
        }
    }
    return $null
}

function Get-OfficialDlssRuntime([string]$CacheRoot, [string]$LocalPath) {
    if ($LocalPath) {
        $info = Assert-PeFile $LocalPath 'NVIDIA DLSS runtime' $true $true $false
        return [pscustomobject]@{ Info = $info; Version = $info.FileVersion; Source = 'Pre-bundled or supplied local file' }
    }

    $release = Get-RemoteJson $script:NvidiaLatestRelease 'the latest NVIDIA DLSS release'
    $tag = [string]$release.tag_name
    if ($tag -notmatch '^v?[0-9A-Za-z._-]+$') { throw "NVIDIA returned an unsafe release tag '$tag'." }
    $uri = "https://raw.githubusercontent.com/NVIDIA/DLSS/refs/tags/$tag/lib/Windows_x86_64/rel/nvngx_dlss.dll"
    $destination = Join-Path $CacheRoot ("nvngx_dlss_$($tag.TrimStart('v')).dll")
    if ($ForceDownload -or -not (Test-Path -LiteralPath $destination -PathType Leaf)) {
        Save-Download $uri $destination "NVIDIA DLSS $tag"
    }
    $info = Assert-PeFile $destination 'NVIDIA DLSS runtime' $true $true $false
    return [pscustomobject]@{ Info = $info; Version = $tag.TrimStart('v'); Source = $uri }
}

function Get-RhiNrRuntime([string]$CacheRoot, [bool]$Extended, [string]$LocalPath) {
    if ($LocalPath) {
        $info = Assert-PeFile $LocalPath 'NVIDIA DLSS Neural Rendering runtime' $true $true $Extended
        return [pscustomobject]@{ Info = $info; Version = $info.FileVersion; Source = 'Pre-bundled or supplied local file' }
    }

    $manifest = Get-RemoteJson $script:RhiManifest 'the RHI runtime manifest'
    $entries = @($manifest.dlssnr)
    if ($Extended) {
        $entry = $entries | Where-Object { [string]$_.version -match '(?i)SF' } | Select-Object -First 1
    }
    else {
        $entry = $entries | Where-Object { [string]$_.version -notmatch '(?i)SF' } | Select-Object -First 1
    }
    if (-not $entry -or -not $entry.url) {
        $kind = if ($Extended) { 'ShortFuse compatibility' } else { 'stock' }
        throw "The RHI manifest does not currently list a $kind DLSS Neural Rendering runtime."
    }

    $safeVersion = ([string]$entry.version) -replace '[^0-9A-Za-z._-]', '_'
    $archive = Join-Path $CacheRoot "nvngx_dlssnr_$safeVersion.zip"
    $destination = Join-Path $CacheRoot "nvngx_dlssnr_$safeVersion.dll"
    if ($ForceDownload -or -not (Test-Path -LiteralPath $destination -PathType Leaf)) {
        Save-Download ([string]$entry.url) $archive "DLSS Neural Rendering $($entry.version) from RHI"
        Copy-ArchivePayload $archive 'nvngx_dlssnr.dll' $destination 'DLSS Neural Rendering'
    }
    $info = Assert-PeFile $destination 'NVIDIA DLSS Neural Rendering runtime' $true $true $Extended
    return [pscustomobject]@{ Info = $info; Version = [string]$entry.version; Source = [string]$entry.url }
}

function Get-PairedRhiRuntimes(
    [string]$CacheRoot,
    [bool]$Extended,
    [string]$LocalDlssPath,
    [string]$LocalNrPath
) {
    if (($LocalDlssPath -and -not $LocalNrPath) -or ($LocalNrPath -and -not $LocalDlssPath)) {
        throw 'A Neural Rendering offline setup must supply both the profile-specific nvngx_dlss.dll and nvngx_dlssnr.dll as a reviewed pair.'
    }
    if ($LocalDlssPath -and $LocalNrPath) {
        $dlssInfo = Assert-PeFile $LocalDlssPath 'NVIDIA DLSS runtime' $true $true $false
        $nrInfo = Assert-PeFile $LocalNrPath 'NVIDIA DLSS Neural Rendering runtime' $true $true $Extended
        $baseMatch = [regex]::Match([string]$dlssInfo.FileVersion, '^(\d+\.\d+)')
        $nrMatch = [regex]::Match([string]$nrInfo.FileVersion, '^(\d+\.\d+)')
        if ($baseMatch.Success -and $nrMatch.Success -and $baseMatch.Groups[1].Value -ne $nrMatch.Groups[1].Value) {
            throw "The supplied DLSS ($($dlssInfo.FileVersion)) and NR ($($nrInfo.FileVersion)) runtimes are from different version families."
        }
        return [pscustomobject]@{
            Dlss = [pscustomobject]@{ Info = $dlssInfo; Version = $dlssInfo.FileVersion; Source = 'Profile-specific local runtime pair' }
            Nr = [pscustomobject]@{ Info = $nrInfo; Version = $nrInfo.FileVersion; Source = 'Profile-specific local runtime pair' }
        }
    }

    $manifest = Get-RemoteJson $script:RhiManifest 'the RHI runtime manifest'
    $nrEntries = @($manifest.dlssnr)
    if ($Extended) {
        $nrEntry = $nrEntries | Where-Object { [string]$_.version -match '(?i)SF' } | Select-Object -First 1
    }
    else {
        $nrEntry = $nrEntries | Where-Object { [string]$_.version -notmatch '(?i)SF' } | Select-Object -First 1
    }
    if (-not $nrEntry -or -not $nrEntry.url) { throw 'RHI does not currently list the requested Neural Rendering tier.' }

    $nrVersion = [string]$nrEntry.version
    $policy = $script:RhiPairPolicy[$nrVersion]
    if (-not $policy) {
        throw "RHI selected unreviewed NR runtime '$nrVersion'. This bootstrap fails closed until its matching base runtime and hashes are audited."
    }
    if ([bool]$policy.Modified -ne $Extended) { throw "Runtime policy for '$nrVersion' does not match the requested profile tier." }
    $baseEntry = @($manifest.dlss) |
        Where-Object { [string]$_.version -eq [string]$policy.BaseVersion } |
        Select-Object -First 1
    if (-not $baseEntry -or -not $baseEntry.url) {
        throw "RHI does not list required base DLSS $($policy.BaseVersion) for NR $nrVersion."
    }

    foreach ($candidate in @([string]$baseEntry.url, [string]$nrEntry.url)) {
        $uri = [Uri]$candidate
        if ($uri.Scheme -ne 'https' -or $uri.Host -ne 'github.com') { throw "RHI returned an unexpected runtime URL: '$candidate'" }
    }

    $baseArchive = Join-Path $CacheRoot "nvngx_dlss_$($policy.BaseVersion)_rhi.zip"
    $baseDll = Join-Path $CacheRoot "nvngx_dlss_$($policy.BaseVersion)_rhi.dll"
    $needBase = $ForceDownload -or -not (Test-Path -LiteralPath $baseDll -PathType Leaf)
    if (-not $needBase) { $needBase = (Get-Sha256 $baseDll) -ne [string]$policy.BaseDllSha256 }
    if ($needBase) {
        Save-Download ([string]$baseEntry.url) $baseArchive "paired DLSS $($policy.BaseVersion) from RHI"
        if ((Get-Sha256 $baseArchive) -ne [string]$policy.BaseArchiveSha256) { throw 'The paired DLSS archive did not match its reviewed SHA-256.' }
        Copy-ArchivePayload $baseArchive 'nvngx_dlss.dll' $baseDll 'paired NVIDIA DLSS runtime'
    }
    if ((Get-Sha256 $baseDll) -ne [string]$policy.BaseDllSha256) { throw 'The paired DLSS DLL did not match its reviewed SHA-256.' }

    $safeNrVersion = $nrVersion -replace '[^0-9A-Za-z._-]', '_'
    $nrArchive = Join-Path $CacheRoot "nvngx_dlssnr_$safeNrVersion.zip"
    $nrDll = Join-Path $CacheRoot "nvngx_dlssnr_$safeNrVersion.dll"
    $needNr = $ForceDownload -or -not (Test-Path -LiteralPath $nrDll -PathType Leaf)
    if (-not $needNr) { $needNr = (Get-Sha256 $nrDll) -ne [string]$policy.NrDllSha256 }
    if ($needNr) {
        Save-Download ([string]$nrEntry.url) $nrArchive "DLSS Neural Rendering $nrVersion from RHI"
        if ((Get-Sha256 $nrArchive) -ne [string]$policy.NrArchiveSha256) { throw 'The DLSS NR archive did not match its reviewed SHA-256.' }
        Copy-ArchivePayload $nrArchive 'nvngx_dlssnr.dll' $nrDll 'DLSS Neural Rendering'
    }
    if ((Get-Sha256 $nrDll) -ne [string]$policy.NrDllSha256) { throw 'The DLSS NR DLL did not match its reviewed SHA-256.' }

    $dlssInfo = Assert-PeFile $baseDll 'NVIDIA DLSS runtime' $true $true $false
    $nrInfo = Assert-PeFile $nrDll 'NVIDIA DLSS Neural Rendering runtime' $true $true $Extended
    return [pscustomobject]@{
        Dlss = [pscustomobject]@{ Info = $dlssInfo; Version = [string]$policy.BaseVersion; Source = [string]$baseEntry.url }
        Nr = [pscustomobject]@{ Info = $nrInfo; Version = $nrVersion; Source = [string]$nrEntry.url }
    }
}
function Get-RenoDxAddon([string]$CacheRoot, [string]$LocalPath) {
    if ($LocalPath) {
        $info = Assert-PeFile $LocalPath 'RenoDX DLSS 5 add-on' $true $false $false
        return [pscustomobject]@{ Info = $info; Version = $info.FileVersion; Source = 'Pre-bundled or supplied local file' }
    }

    # Windows PowerShell 5.1 exposes a top-level JSON array as one Object[] pipeline item.
    # Wrapping this call in @() would therefore create a nested array.
    $releases = Get-RemoteJson $script:RhiRepoReleases 'RHI''s RenoDX component releases'
    $release = $releases |
        Where-Object { -not $_.draft -and [string]$_.tag_name -like 'renodx-dlss5-*' } |
        Sort-Object { [DateTime]$_.published_at } -Descending |
        Select-Object -First 1
    if (-not $release) { throw 'No RenoDX DLSS 5 component release was found in the RHI repository.' }
    $asset = $release.assets |
        Where-Object { $_.name -ieq 'renodx-dlss5.addon64' -or $_.name -match '(?i)^renodx-dlss5.*\.zip$' } |
        Select-Object -First 1
    if (-not $asset) { throw "RHI release '$($release.tag_name)' has no RenoDX DLSS 5 add-on asset." }

    $version = ([string]$release.tag_name) -replace '^renodx-dlss5-', ''
    $safeVersion = $version -replace '[^0-9A-Za-z._-]', '_'
    $destination = Join-Path $CacheRoot "renodx-dlss5_$safeVersion.addon64"
    if ($ForceDownload -or -not (Test-Path -LiteralPath $destination -PathType Leaf)) {
        if ([string]$asset.name -match '(?i)\.zip$') {
            $archive = Join-Path $CacheRoot "renodx-dlss5_$safeVersion.zip"
            Save-Download ([string]$asset.browser_download_url) $archive "RenoDX DLSS 5 add-on $version from RHI"
            Copy-ArchivePayload $archive 'renodx-dlss5.addon64' $destination 'RenoDX DLSS 5 add-on'
        }
        else {
            Save-Download ([string]$asset.browser_download_url) $destination "RenoDX DLSS 5 add-on $version from RHI"
        }
    }
    $info = Assert-PeFile $destination 'RenoDX DLSS 5 add-on' $true $false $false
    return [pscustomobject]@{ Info = $info; Version = $version; Source = [string]$asset.browser_download_url }
}

function Get-ComponentRecord([object]$Component) {
    if (-not $Component) { return $null }
    return [ordered]@{
        Version = $Component.Version
        Source = $Component.Source
        Sha256 = $Component.Info.Sha256
        FileVersion = $Component.Info.FileVersion
        SignatureStatus = $Component.Info.SignatureStatus
        Signer = $Component.Info.Signer
    }
}

function Save-PreviousProfileConfig([string]$ActiveRoot, [string]$ProfileRoot) {
    $statePath = Join-Path $ActiveRoot 'CK3-DLSS-RUNTIME.json'
    $configPath = Join-Path $ActiveRoot 'dlss5-feed.cfg'
    if (-not (Test-Path -LiteralPath $statePath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $configPath -PathType Leaf)) { return }
    try {
        $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
        $oldMode = [string]$state.Profile
        if ($oldMode -notmatch '^[0-9A-Za-z_-]+$') { return }
        $destination = Join-Path (Join-Path $ProfileRoot $oldMode) 'dlss5-feed.cfg'
        Ensure-Directory (Split-Path -Parent $destination)
        Copy-Item -LiteralPath $configPath -Destination $destination -Force
    }
    catch {
        Write-Warning "Could not preserve the previous feeder profile: $($_.Exception.Message)"
    }
}

function New-DefaultFeederConfig([string]$SelectedMode) {
    $delay = if ($SelectedMode -eq 'DLSS45') { 0 } else { 60 }
    $warmup = if ($SelectedMode -eq 'DLSS45') { 0 } else { 180 }
    # Model M explicitly enables the requested DLSS 4.5 test. It works with DLAA on RTX 3060,
    # although RTX 20/30 cards lack native FP8 and can pay a larger performance cost than K.
    $preset = if ($SelectedMode -eq 'DLSS45') { 13 } else { 0 }
    return @"
# CK3 portable profile: $SelectedMode
# mode=2 is the full NVIDIA NGX DLSS/DLAA path. mode=1 is only a transport diagnostic.
# Presets: 11=K (recommended DLAA), 12=L (4.5 Ultra Performance model), 13=M (4.5 Performance model).
enabled=1
mode=2
hdr=-1
depth_inverted=-1
flags=-1
reset_every=0
warmup_rebuild=$warmup
rebuild=0
log_frames=3
create_delay=$delay
preset=$preset
mv_scale_x=1.000
mv_scale_y=1.000
"@
}

function Install-ActiveProfile(
    [string]$SelectedMode,
    [string]$BinaryRoot,
    [string]$FeederPath,
    [object]$Dlss,
    [object]$Nr,
    [object]$Reno,
    [string[]]$GpuNames
) {
    $activeRoot = Join-Path $BinaryRoot 'dlss-active'
    $cacheRoot = Join-Path $BinaryRoot 'dlss-cache'
    $profileRoot = Join-Path $cacheRoot 'profiles'
    Ensure-Directory $activeRoot
    Ensure-Directory $profileRoot
    Save-PreviousProfileConfig $activeRoot $profileRoot

    foreach ($name in @('dlss5-feed.addon64', 'renodx-dlss5.addon64', 'nvngx_dlss.dll', 'nvngx_dlssnr.dll', 'CK3-DLSS-RUNTIME.json')) {
        $target = Join-Path $activeRoot $name
        if (Test-Path -LiteralPath $target -PathType Leaf) { Remove-Item -LiteralPath $target -Force }
    }

    Copy-Item -LiteralPath $FeederPath -Destination (Join-Path $activeRoot 'dlss5-feed.addon64') -Force
    Copy-Item -LiteralPath $Dlss.Info.Path -Destination (Join-Path $activeRoot 'nvngx_dlss.dll') -Force
    if ($SelectedMode -ne 'DLSS45') {
        Copy-Item -LiteralPath $Nr.Info.Path -Destination (Join-Path $activeRoot 'nvngx_dlssnr.dll') -Force
        Copy-Item -LiteralPath $Reno.Info.Path -Destination (Join-Path $activeRoot 'renodx-dlss5.addon64') -Force
    }

    $profileConfig = Join-Path (Join-Path $profileRoot $SelectedMode) 'dlss5-feed.cfg'
    $activeConfig = Join-Path $activeRoot 'dlss5-feed.cfg'
    if (Test-Path -LiteralPath $profileConfig -PathType Leaf) {
        Copy-Item -LiteralPath $profileConfig -Destination $activeConfig -Force
    }
    else {
        Ensure-Directory (Split-Path -Parent $profileConfig)
        $config = New-DefaultFeederConfig $SelectedMode
        [IO.File]::WriteAllText($profileConfig, $config, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText($activeConfig, $config, [Text.UTF8Encoding]::new($false))
    }

    $displayName = switch ($SelectedMode) {
        'DLSS45' { 'DLSS 4.5 Model M Neural Reconstruction (DLAA)' }
        'DLSS5' { 'DLSS 5 Neural Rendering' }
        'DLSS5Extended' { 'DLSS 5 Extended (experimental RHI/ShortFuse)' }
    }
    $feederInfo = Assert-PeFile (Join-Path $activeRoot 'dlss5-feed.addon64') 'DLSS feeder add-on' $true $false $false
    $state = [ordered]@{
        SchemaVersion = 1
        Profile = $SelectedMode
        DisplayName = $displayName
        InstalledUtc = [DateTime]::UtcNow.ToString('o')
        DetectedGpus = @($GpuNames)
        Experimental = $SelectedMode -eq 'DLSS5Extended'
        Components = [ordered]@{
            Feeder = [ordered]@{ Source = 'Package payload'; Sha256 = $feederInfo.Sha256; FileVersion = $feederInfo.FileVersion }
            Dlss = Get-ComponentRecord $Dlss
            DlssNr = Get-ComponentRecord $Nr
            RenoDx = Get-ComponentRecord $Reno
        }
        Sources = [ordered]@{
            NvidiaLicense = $script:NvidiaLicense
            RhiManifest = $script:RhiManifest
            RhiProject = $script:RhiProject
        }
    }
    $json = $state | ConvertTo-Json -Depth 7
    [IO.File]::WriteAllText((Join-Path $activeRoot 'CK3-DLSS-RUNTIME.json'), $json + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))

    Write-RuntimeStatus "Activated $displayName."
    if ($SelectedMode -eq 'DLSS45') {
        Write-RuntimeStatus 'Model M neural reconstruction is active. RenoDX and the separate DLSS 5 nvngx_dlssnr.dll extension are not loaded.'
    }
    elseif ($SelectedMode -eq 'DLSS5Extended') {
        Write-Warning 'This profile uses a modified compatibility runtime. Expect instability; DLSS 4.5 Model M neural reconstruction is the safe RTX 3060 baseline.'
    }
}

function Test-ActiveProfile([string]$BinaryRoot) {
    $activeRoot = Join-Path $BinaryRoot 'dlss-active'
    $statePath = Join-Path $activeRoot 'CK3-DLSS-RUNTIME.json'
    if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) {
        throw 'No active runtime profile exists. Run Install CK3 DLSS.cmd first.'
    }
    try {
        $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    }
    catch {
        throw "The active runtime state file is invalid: $($_.Exception.Message)"
    }
    $profile = [string]$state.Profile
    if ($profile -notin @('DLSS45', 'DLSS5', 'DLSS5Extended')) {
        throw "The active runtime profile '$profile' is not recognized."
    }

    $required = @('dlss5-feed.addon64', 'nvngx_dlss.dll', 'dlss5-feed.cfg')
    if ($profile -ne 'DLSS45') { $required += @('renodx-dlss5.addon64', 'nvngx_dlssnr.dll') }
    $missing = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $activeRoot $_) -PathType Leaf) })
    if ($missing.Count) { throw "Active profile '$profile' is missing: $($missing -join ', ')" }
    if ($profile -eq 'DLSS45' -and (Test-Path -LiteralPath (Join-Path $activeRoot 'renodx-dlss5.addon64') -PathType Leaf)) {
        throw 'The DLSS 4.5 profile is unsafe: the RenoDX DLSS 5 add-on is still active.'
    }

    Assert-PeFile (Join-Path $activeRoot 'dlss5-feed.addon64') 'DLSS feeder add-on' $true $false $false | Out-Null
    $allowModified = $profile -eq 'DLSS5Extended'
    Assert-PeFile (Join-Path $activeRoot 'nvngx_dlss.dll') 'NVIDIA DLSS runtime' $true $true $false | Out-Null
    if ($profile -ne 'DLSS45') {
        Assert-PeFile (Join-Path $activeRoot 'renodx-dlss5.addon64') 'RenoDX DLSS 5 add-on' $true $false $false | Out-Null
        Assert-PeFile (Join-Path $activeRoot 'nvngx_dlssnr.dll') 'NVIDIA DLSS Neural Rendering runtime' $true $true $allowModified | Out-Null
    }
    Write-RuntimeStatus "Active profile: $($state.DisplayName)"
    Write-RuntimeStatus "Runtime metadata: $statePath"
    return $state
}

function Open-RhiManager([string]$CacheRoot) {
    $bundledSetup = Join-Path $root 'tools\RHI-Setup.exe'
    if (Test-Path -LiteralPath $bundledSetup -PathType Leaf) {
        Assert-PeFile $bundledSetup 'Bundled RHI setup' $false $false $false | Out-Null
        Write-RuntimeStatus "Opening bundled RHI setup '$bundledSetup'."
        Start-Process -FilePath $bundledSetup
        return
    }

    if (-not $AcceptRuntimeLicenses) {
        Write-Host ''
        Write-Host 'This downloads and opens the latest official RHI installer from RankFTW/RHI.' -ForegroundColor Yellow
        Write-Host 'RHI is optional; this CK3 package already acquires its runtime components itself.'
        Write-Host 'Do not enable RHI''s global Vulkan ReShade for CK3 while using this portable launcher;' -ForegroundColor Yellow
        Write-Host 'both expose VK_LAYER_reshade and would conflict.' -ForegroundColor Yellow
        $answer = (Read-Host 'Type OPEN RHI to continue').Trim()
        if ($answer -cne 'OPEN RHI') { throw 'RHI Manager was not downloaded.' }
    }
    $release = Get-RemoteJson $script:RhiReleases 'the latest RHI release'
    $asset = @($release.assets) |
        Where-Object { $_.name -match '(?i)^RHI.*Setup.*\.exe$' -or $_.name -ieq 'RHI-Setup.exe' } |
        Select-Object -First 1
    if (-not $asset) { throw "RHI release '$($release.tag_name)' has no setup executable asset." }
    $safeTag = ([string]$release.tag_name) -replace '[^0-9A-Za-z._-]', '_'
    $destination = Join-Path (Join-Path $CacheRoot 'rhi-manager') "RHI-Setup-$safeTag.exe"
    if ($ForceDownload -or -not (Test-Path -LiteralPath $destination -PathType Leaf)) {
        Save-Download ([string]$asset.browser_download_url) $destination "RHI $($release.tag_name) setup"
    }
    Assert-PeFile $destination 'RHI setup' $false $false $false | Out-Null
    Write-RuntimeStatus "Opening '$destination'."
    Start-Process -FilePath $destination
}

$root = Resolve-PackageRoot $GameRoot
$binaryRoot = Join-Path $root 'binaries'
$payloadRoot = Join-Path $binaryRoot 'dlss-payload'
$cacheRoot = Join-Path $binaryRoot 'dlss-cache\downloads'
Ensure-Directory $cacheRoot

switch ($Action) {
    'Status' {
        Test-ActiveProfile $binaryRoot | Out-Null
    }
    'OpenRHI' {
        Open-RhiManager $cacheRoot
    }
    'Configure' {
        $gpuNames = @(Get-GpuNames)
        $selectedMode = if ($Mode -eq 'Auto') { Select-RuntimeMode $gpuNames } else { $Mode }

        $feederPath = Find-LocalFile '' @(
            (Join-Path $payloadRoot 'dlss5-feed.addon64'),
            (Join-Path $binaryRoot 'dlss5-feed.addon64'),
            (Join-Path $binaryRoot 'dlss-active\dlss5-feed.addon64')
        ) 'DLSS feeder add-on'
        if (-not $feederPath) { throw 'DLSS feeder payload is missing. Rebuild or re-extract the package.' }
        Assert-PeFile $feederPath 'DLSS feeder add-on' $true $false $false | Out-Null

        $dlssLocal = Find-LocalFile $DlssRuntime @(
            (Join-Path $payloadRoot "runtimes\$selectedMode\nvngx_dlss.dll")
        ) 'NVIDIA DLSS runtime'
        $nrLocal = $null
        $renoLocal = $null
        if ($selectedMode -ne 'DLSS45') {
            $nrLocal = Find-LocalFile $DlssNrRuntime @(
                (Join-Path $payloadRoot "runtimes\$selectedMode\nvngx_dlssnr.dll")
            ) 'NVIDIA DLSS Neural Rendering runtime'
            $renoLocal = Find-LocalFile $RenoDxAddon @(
                (Join-Path $payloadRoot "runtimes\$selectedMode\renodx-dlss5.addon64"),
                (Join-Path $payloadRoot 'runtimes\shared\renodx-dlss5.addon64')
            ) 'RenoDX DLSS 5 add-on'
        }

        $needsDownload = -not $dlssLocal -or ($selectedMode -ne 'DLSS45' -and (-not $nrLocal -or -not $renoLocal))
        if ($needsDownload) { Confirm-Downloads $selectedMode }

        $dlss = $null
        $nr = $null
        $reno = $null
        if ($selectedMode -eq 'DLSS45') {
            $dlss = Get-OfficialDlssRuntime $cacheRoot $dlssLocal
        }
        else {
            $pair = Get-PairedRhiRuntimes $cacheRoot ($selectedMode -eq 'DLSS5Extended') $dlssLocal $nrLocal
            $dlss = $pair.Dlss
            $nr = $pair.Nr
            $reno = Get-RenoDxAddon $cacheRoot $renoLocal
        }
        Install-ActiveProfile $selectedMode $binaryRoot $feederPath $dlss $nr $reno $gpuNames
        Test-ActiveProfile $binaryRoot | Out-Null
    }
}

