<#
.SYNOPSIS
    Install the shader packages from crosire's official list (the one the ReShade installer offers)
    into a reshade-shaders folder.

.DESCRIPTION
    Reads https://raw.githubusercontent.com/crosire/reshade-shaders/list/EffectPackages.ini and, for
    every package, does what ReShade's setup does: download the archive, take the shallowest folder
    holding .fx files as the effect root and the first "Textures" folder as the texture root, copy
    them to the package's InstallPath / TextureInstallPath (relative to the reshade-shaders folder)
    and drop its DenyEffectFiles.

    Existing files are kept unless -Force: the root ReShade.fxh / ReShadeUI.fxh shipped with the
    feeder, DLSS5_Feed.fx and anything you edited stay as they are. Archives are cached, so a rerun
    only downloads what is missing (-Refresh downloads again, for package updates).

.EXAMPLE
    .\Install-ReShadeShaders.ps1 -Destination "$env:USERPROFILE\DLSS5-Feeder\ElgatoStudio\reshade-shaders"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Destination,
    # LumeniteFX: the feeder deploys its own copy at the Shaders root; a second one under
    # Shaders\LumeniteFX would duplicate lumenite_Kernel.fx (the motion-vector provider).
    [string[]] $Exclude = @('LumeniteFX'),
    [string] $Cache = (Join-Path $env:LOCALAPPDATA 'DLSS5-Feeder\downloads\shader-packages'),
    [switch] $Force,
    [switch] $Refresh
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
New-Item -ItemType Directory -Force $Cache, $Destination | Out-Null

$listFile = Join-Path $Cache 'EffectPackages.ini'
Invoke-WebRequest 'https://raw.githubusercontent.com/crosire/reshade-shaders/list/EffectPackages.ini' -OutFile $listFile -UseBasicParsing

$packages = @(); $cur = $null
foreach ($line in Get-Content $listFile) {
    if ($line -match '^\[(.+)\]$') { if ($cur) { $packages += [pscustomobject]$cur }; $cur = @{ Id = $Matches[1] }; continue }
    if ($cur -and $line -match '^([^=]+)=(.*)$') { $cur[$Matches[1].Trim()] = $Matches[2].Trim() }
}
if ($cur) { $packages += [pscustomobject]$cur }

function Resolve-Under([string] $rel) {
    # list paths look like .\reshade-shaders\Shaders\X; make them relative to -Destination
    $r = $rel -replace '^\.\\', '' -replace '^reshade-shaders\\?', ''
    Join-Path $Destination $r
}
function Copy-Tree([string] $from, [string] $to, [string[]] $deny) {
    $n = 0
    $base = (Resolve-Path $from).Path.TrimEnd('\')
    foreach ($f in Get-ChildItem $base -Recurse -File) {
        if ($deny -contains $f.Name) { continue }
        $dst = Join-Path $to $f.FullName.Substring($base.Length + 1)
        if ((Test-Path $dst) -and -not $Force) { continue }
        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
        Copy-Item $f.FullName $dst -Force; $n++
    }
    $n
}

$tmpRoot = Join-Path ([IO.Path]::GetTempPath()) ('reshade-pkgs-' + [guid]::NewGuid().ToString('N'))
$ok = 0; $failed = @()
foreach ($p in $packages) {
    $short = ($p.InstallPath -split '\\')[-1]
    if ($Exclude -contains $short -or $Exclude -contains $p.PackageName) { Write-Host "  skip  $($p.PackageName)"; continue }
    try {
        $zip = Join-Path $Cache (($p.Id + '-' + ($p.DownloadUrl -replace '^https?://', '' -replace '[^\w.-]', '_')) + '')
        if ($Refresh -or -not (Test-Path $zip)) { Invoke-WebRequest $p.DownloadUrl -OutFile $zip -UseBasicParsing }
        $tmp = Join-Path $tmpRoot $p.Id
        Expand-Archive $zip $tmp -Force
        $fxDir = Get-ChildItem $tmp -Recurse -File -Filter *.fx | ForEach-Object DirectoryName |
            Sort-Object Length | Select-Object -First 1
        if (-not $fxDir) { throw 'no .fx files in the archive' }
        $deny = @(); if ($p.DenyEffectFiles) { $deny = $p.DenyEffectFiles -split ',' | ForEach-Object Trim }
        $n = Copy-Tree $fxDir (Resolve-Under $p.InstallPath) $deny
        $tex = Get-ChildItem $tmp -Recurse -Directory -Filter Textures | Select-Object -First 1
        if ($tex -and $p.TextureInstallPath) { $n += Copy-Tree $tex.FullName (Resolve-Under $p.TextureInstallPath) @() }
        Write-Host ('  ok    {0,-45} {1,5} new files' -f $p.PackageName, $n); $ok++
    } catch {
        Write-Host "  FAIL  $($p.PackageName): $($_.Exception.Message)" -ForegroundColor Yellow; $failed += $p.PackageName
    }
}
Remove-Item -Recurse -Force $tmpRoot -ErrorAction SilentlyContinue

# Same-named effects in different packages show up twice in ReShade's list and make presets
# ambiguous (technique@file); report them, the installer has the same behaviour.
$dupes = Get-ChildItem (Join-Path $Destination 'Shaders') -Recurse -File -Filter *.fx | Group-Object Name | Where-Object Count -gt 1
$fx = (Get-ChildItem (Join-Path $Destination 'Shaders') -Recurse -File -Filter *.fx).Count
Write-Host "$ok packages installed, $($failed.Count) failed; $fx effect files in $Destination"
if ($dupes) { Write-Host "Same-named effects in several packages: $(($dupes | ForEach-Object Name) -join ', ')" }
