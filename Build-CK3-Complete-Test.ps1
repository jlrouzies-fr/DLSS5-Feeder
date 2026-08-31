[CmdletBinding()]
param(
    [string]$OutputDirectory = '',
    [string]$DeployTo,
    [ValidateSet('DLSS45', 'DLSS5', 'DLSS5Extended')]
    [string]$InstallProfile = 'DLSS45',
    [string]$SettingsPath,
    [switch]$InstallAfterDeploy,
    [switch]$ForceDownload
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $scriptRoot 'release\CK3-DLSS-Complete-Test' }

$script:Headers = @{ 'User-Agent' = 'CK3-DLSS-Complete-Private-Test' }
$script:Expected = @{
    Dlss45 = 'be6e434a94ca32499515eb62ca0e6c274526055d568d0426e4c652dcdfb6ee6e'
    Dlss3108Archive = 'fb481660f7e952b87f91760e3afd7f9dc14cd2c3361b470e948d6346e4323009'
    Dlss3108 = 'c85f971ce023c9f3492fc7455f0b01a24ba18ea39636407a846902c4360b0b7e'
    NrStockArchive = '388c0a7912e15ec911b9c9e11a692142b11fe387ddf2b637d8c358138fffb3ac'
    NrStock = 'e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e'
    NrExtendedArchive = '1da35941894994eb087e017577829e492454e9bae3a6a9397027069ceb74955c'
    NrExtended = '6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927'
    RenoDx = '9150097cdee2953cdc9894d2e5606ea5100e6c8f95fc7bb1b407328b4391a07a'
    RenoDxArchive = '15481c492db76682e9a88917e7f78897351ecf088bfae9bca74a0c5b74ddd033'
}

function Write-Step([string]$Message) {
    Write-Host "[complete CK3 test bundle] $Message"
}

function Ensure-Directory([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { New-Item -ItemType Directory -Path $Path -Force | Out-Null }
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-Hash([string]$Path, [string]$Expected, [string]$Label) {
    $actual = Get-Sha256 $Path
    if ($actual -ne $Expected) { throw "$Label hash mismatch. Expected $Expected, received $actual." }
}

function Download([string]$Uri, [string]$Destination, [string]$Label, [string]$ExpectedHash = '') {
    Write-Step "Downloading $Label..."
    Invoke-WebRequest -Uri $Uri -OutFile $Destination -UseBasicParsing -Headers $script:Headers
    if ($ExpectedHash) { Assert-Hash $Destination $ExpectedHash $Label }
    return $Destination
}

function Expand-One([string]$Archive, [string]$Name, [string]$Destination, [string]$ExpectedHash) {
    $extract = "$Archive.extract"
    if (Test-Path -LiteralPath $extract -PathType Container) { Remove-Item -LiteralPath $extract -Recurse -Force }
    Ensure-Directory $extract
    try {
        Expand-Archive -LiteralPath $Archive -DestinationPath $extract -Force
        $matches = @(Get-ChildItem -LiteralPath $extract -Recurse -File -Filter $Name)
        if ($matches.Count -ne 1) { throw "Expected one $Name in '$Archive'; found $($matches.Count)." }
        Copy-Item -LiteralPath $matches[0].FullName -Destination $Destination -Force
        Assert-Hash $Destination $ExpectedHash $Name
    }
    finally {
        if (Test-Path -LiteralPath $extract -PathType Container) { Remove-Item -LiteralPath $extract -Recurse -Force }
    }
}

$output = [IO.Path]::GetFullPath($OutputDirectory)
$releaseRoot = [IO.Path]::GetFullPath((Join-Path $scriptRoot 'release'))
if (-not $output.StartsWith($releaseRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputDirectory must remain below '$releaseRoot'."
}

$temp = Join-Path ([IO.Path]::GetTempPath()) ('ck3-dlss-complete-' + [guid]::NewGuid().ToString('N'))
Ensure-Directory $temp
try {
    $feeder = Download 'https://github.com/jlrouzies-fr/DLSS5-Feeder/releases/latest/download/dlss5-feed.addon64' `
        (Join-Path $temp 'dlss5-feed.addon64') 'DLSS5 Feeder add-on'
    $layer = Download 'https://github.com/jlrouzies-fr/DLSS5-Feeder/releases/latest/download/feed-vk-layer.zip' `
        (Join-Path $temp 'feed-vk-layer.zip') 'feeder Vulkan layer'
    $dlss45 = Download 'https://raw.githubusercontent.com/NVIDIA/DLSS/refs/tags/v310.7.0/lib/Windows_x86_64/rel/nvngx_dlss.dll' `
        (Join-Path $temp 'nvngx_dlss_310.7.0.dll') 'NVIDIA DLSS 310.7.0' $script:Expected.Dlss45

    $baseArchive = Download 'https://github.com/RankFTW/rhi-repo/releases/download/dlss-310.8.0/nvngx_dlss_310.8.0.zip' `
        (Join-Path $temp 'nvngx_dlss_310.8.0.zip') 'paired DLSS 310.8.0' $script:Expected.Dlss3108Archive
    $base3108 = Join-Path $temp 'nvngx_dlss_310.8.0.dll'
    Expand-One $baseArchive 'nvngx_dlss.dll' $base3108 $script:Expected.Dlss3108

    $stockArchive = Download 'https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.8.0/nvngx_dlssnr_310.8.0.zip' `
        (Join-Path $temp 'nvngx_dlssnr_310.8.0.zip') 'signed stock NR 310.8.0' $script:Expected.NrStockArchive
    $stockNr = Join-Path $temp 'nvngx_dlssnr_310.8.0.dll'
    Expand-One $stockArchive 'nvngx_dlssnr.dll' $stockNr $script:Expected.NrStock

    $extendedArchive = Download 'https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.8.SF-v2/nvngx_dlssnr_310.8.SF-v2.zip' `
        (Join-Path $temp 'nvngx_dlssnr_310.8.SF-v2.zip') 'ShortFuse NR 310.8.SF-v2' $script:Expected.NrExtendedArchive
    $extendedNr = Join-Path $temp 'nvngx_dlssnr_310.8.SF-v2.dll'
    Expand-One $extendedArchive 'nvngx_dlssnr.dll' $extendedNr $script:Expected.NrExtended

    $renoArchive = Download 'https://github.com/RankFTW/rhi-repo/releases/download/renodx-dlss5-4.55/renodx-dlss5_4.55.zip' `
        (Join-Path $temp 'renodx-dlss5_4.55.zip') 'RenoDX DLSS 5 add-on 4.55' $script:Expected.RenoDxArchive
    $reno = Join-Path $temp 'renodx-dlss5.addon64'
    Expand-One $renoArchive 'renodx-dlss5.addon64' $reno $script:Expected.RenoDx

    & (Join-Path $scriptRoot 'Build-CK3-Package.ps1') `
        -FeederAddon $feeder `
        -FeedLayerZip $layer `
        -Dlss45Runtime $dlss45 `
        -Dlss5Runtime $base3108 `
        -Dlss5NrRuntime $stockNr `
        -Dlss5ExtendedRuntime $base3108 `
        -Dlss5ExtendedNrRuntime $extendedNr `
        -RenoDxAddon $reno `
        -OutputDirectory $output `
        -AcknowledgeRuntimeRedistributionTerms `
        -AllowUpstreamFeederForTesting `
        -KeepStagingDirectory

    # Materialize ReShade, ReShade.fxh and VORT into the bundle. The temporary ck3.exe only
    # satisfies the package-root detector and is removed before the ZIP is rebuilt.
    $fakeGame = Join-Path $output 'binaries\ck3.exe'
    Copy-Item -LiteralPath (Join-Path $output 'binaries\dlss-payload\dlss5-feed.addon64') -Destination $fakeGame -Force
    try {
        & (Join-Path $output 'Graphics-Dependency-Setup.ps1') `
            -Action Install `
            -GameRoot $output `
            -CacheRoot (Join-Path $temp 'graphics-cache') `
            -AcceptDependencyLicenses `
            -ForceDownload:$ForceDownload
    }
    finally {
        if (Test-Path -LiteralPath $fakeGame -PathType Leaf) { Remove-Item -LiteralPath $fakeGame -Force }
    }

    Write-Step 'Downloading the optional RHI setup tool...'
    $rhiRelease = Invoke-RestMethod -Uri 'https://api.github.com/repos/RankFTW/RHI/releases/latest' -Headers $script:Headers -UseBasicParsing
    $rhiAsset = @($rhiRelease.assets) | Where-Object { $_.name -match '(?i)^RHI.*Setup.*\.exe$' -or $_.name -ieq 'RHI-Setup.exe' } | Select-Object -First 1
    if (-not $rhiAsset) { throw "RHI release '$($rhiRelease.tag_name)' has no setup executable." }
    $toolsRoot = Join-Path $output 'tools'
    Ensure-Directory $toolsRoot
    Download ([string]$rhiAsset.browser_download_url) (Join-Path $toolsRoot 'RHI-Setup.exe') "RHI $($rhiRelease.tag_name) setup" | Out-Null

    $description = @"
CK3 DLSS complete private test bundle

Included locally:
- ReShade full-add-on Vulkan DLL
- DLSS5 Feeder add-on and Vulkan layer
- NVIDIA DLSS 310.7.0 (DLSS 4.5 / Model M test profile)
- RHI DLSS 310.8.0 + signed stock NR 310.8.0
- RHI DLSS 310.8.0 + ShortFuse NR 310.8.SF-v2
- RenoDX DLSS 5 add-on 4.55
- ReShade.fxh and VORT motion-vector shaders
- RHI setup tool

Extract over Crusader Kings III and run one of the Install CK3 DLSS *.cmd files.
"@
    [IO.File]::WriteAllText((Join-Path $output 'COMPLETE-TEST-BUNDLE.txt'), $description, [Text.UTF8Encoding]::new($false))

    $zipPath = "$output.zip"
    if (Test-Path -LiteralPath $zipPath -PathType Leaf) { Remove-Item -LiteralPath $zipPath -Force }
    Compress-Archive -Path (Join-Path $output '*') -DestinationPath $zipPath -CompressionLevel Optimal
    Write-Step "Created complete bundle '$zipPath'."

    if ($DeployTo) {
        $target = [IO.Path]::GetFullPath($DeployTo.TrimEnd('\', '/'))
        if (-not (Test-Path -LiteralPath (Join-Path $target 'binaries\ck3.exe') -PathType Leaf)) {
            throw "Deploy target is not a CK3 directory: '$target'"
        }
        Write-Step "Deploying the complete bundle to '$target'..."
        Get-ChildItem -LiteralPath $output -Force | Copy-Item -Destination $target -Recurse -Force
        if ($InstallAfterDeploy) {
            $arguments = @{
                Action = 'Install'
                Profile = $InstallProfile
                GameRoot = $target
                AcceptDependencyLicenses = $true
                AcceptRuntimeLicenses = $true
            }
            if ($SettingsPath) { $arguments.SettingsPath = $SettingsPath }
            & (Join-Path $target 'DLSS5-CK3.ps1') @arguments
            if ($LASTEXITCODE -ne 0) { throw "The deployed installer exited with code $LASTEXITCODE." }
        }
    }
}
finally {
    if (Test-Path -LiteralPath $temp -PathType Container) { Remove-Item -LiteralPath $temp -Recurse -Force }
}

