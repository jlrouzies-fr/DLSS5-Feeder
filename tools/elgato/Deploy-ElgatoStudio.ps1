<#
.SYNOPSIS
    (Re)deploy the feeder + custom WinUI 3 ReShade + OptiScaler DLSS-NR fork into Elgato Studio.

.DESCRIPTION
    Elgato Studio is an MSIX app: every update installs into a NEW version-stamped folder under
    C:\Program Files\WindowsApps and deletes the old one, with everything we put in it. This
    script makes that a one-command redeploy and keeps the user's state OUT of the package folder:

      * reshade-shaders\ and ReShadePreset.ini live in the store folder (default
        %USERPROFILE%\DLSS5-Feeder\ElgatoStudio -- not Documents, often OneDrive); ReShade.ini points at them with absolute paths, so
        shaders and presets you add survive updates on their own.
      * ReShade.ini and OptiScaler.ini must sit next to the DLLs. -Pull copies them back to
        <store>\saved\ (run it after tuning); a deploy always pulls first and then restores
        them, re-forcing the WinUI-critical OptiScaler keys from OptiScaler.overrides.ini.

    Binaries come from the repo checkout (all rebuildable, see docs/STATUS-ELGATO-STUDIO.md):
      dxgi.dll           deploy\reshade-winui3\dxgi_x64.dll  (ReShade 6.8.0 + reshade-6.8.0-winui3.patch)
      dlss5-feed.addon64 build\
      nvngx_dlss*.dll    deploy\shared\
      winmm.dll          deploy\optiscaler-fork\ (jlrouzies-fr fork, fix/finished-picture-reshade)

    Elgato refuses to run outside its package (exit 0x80073D54, no package identity), so a plain
    copy of the folder elsewhere is not a usable target; the package folder needs one elevated
    run to take ownership (the script re-launches itself elevated when it cannot write).

.EXAMPLE
    .\Deploy-ElgatoStudio.ps1            # deploy into the newest installed Elgato Studio
.EXAMPLE
    .\Deploy-ElgatoStudio.ps1 -Pull      # save live ReShade.ini / OptiScaler.ini to the store
#>
[CmdletBinding()]
param(
    [string] $Target,
    [string] $Store = (Join-Path $env:USERPROFILE 'DLSS5-Feeder\ElgatoStudio'),
    [string] $OptiScalerDir,
    [switch] $Pull,
    # also install crosire's full shader package list (tools\Install-ReShadeShaders.ps1) into the store
    [switch] $AllShaders,
    [switch] $NoElevate,
    [string] $Owner = "$env:USERDOMAIN\$env:USERNAME"
)
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $OptiScalerDir) { $OptiScalerDir = Join-Path $Repo 'deploy\optiscaler-fork' }
$Utf8 = New-Object System.Text.UTF8Encoding($false)

function Say([string] $t) { Write-Host "  $t" }

if (-not $Target) {
    # a dev-registered loose copy (Add-AppxPackage -Register) is the only writable layout since 3184
    $Target = (Get-AppxPackage Elgato.Studio -ErrorAction SilentlyContinue | Select-Object -First 1).InstallLocation
}
if (-not $Target) {
    $Target = Get-ChildItem 'C:\Program Files\WindowsApps' -Directory -Filter 'Elgato.Studio_*_x64__g54w8ztgkx496' -ErrorAction SilentlyContinue |
        Sort-Object { [version]($_.Name -split '_')[1] } | Select-Object -Last 1 -ExpandProperty FullName
    if (-not $Target) { throw 'No Elgato Studio package folder found under C:\Program Files\WindowsApps.' }
}
if (-not (Test-Path (Join-Path $Target 'Elgato.Studio.exe'))) { throw "Not an Elgato Studio folder: $Target" }
Write-Host "Elgato Studio: $Target"
Write-Host "Store:         $Store"
New-Item -ItemType Directory -Force $Store | Out-Null
if ($NoElevate) { Start-Transcript -Path (Join-Path $Store 'deploy.log') -Force | Out-Null }   # the elevated window closes

$Saved = Join-Path $Store 'saved'

function Invoke-Pull {
    New-Item -ItemType Directory -Force $Saved | Out-Null
    foreach ($n in 'ReShade.ini', 'OptiScaler.ini') {
        $src = Join-Path $Target $n
        if (-not (Test-Path $src)) { continue }
        $dst = Join-Path $Saved $n
        if ((Test-Path $dst) -and ((Get-FileHash $src).Hash -eq (Get-FileHash $dst).Hash)) { Say "$n unchanged"; continue }
        if (Test-Path $dst) {
            $bak = Join-Path $Saved 'history'
            New-Item -ItemType Directory -Force $bak | Out-Null
            Copy-Item $dst (Join-Path $bak ("{0}.{1:yyyyMMdd-HHmmss}" -f $n, (Get-Item $dst).LastWriteTime))
        }
        Copy-Item $src $dst -Force
        Say "saved $n"
    }
}

if ($Pull) { Invoke-Pull; return }

# --- write access (WindowsApps needs takeown once per package version) ---
function Test-Writable([string] $dir) {
    $p = Join-Path $dir ('_dlss5_write_test_{0}.tmp' -f [guid]::NewGuid().ToString('N'))
    try { [IO.File]::WriteAllText($p, ''); Remove-Item $p; $true } catch { $false }
}
if (-not (Test-Writable $Target)) {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole('Administrators')
    if (-not $admin) {
        if ($NoElevate) { throw "Cannot write to $Target and -NoElevate was given." }
        Write-Host 'Package folder is read-only: re-launching elevated (UAC prompt) ...'
        $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Target', "`"$Target`"",
               '-Store', "`"$Store`"", '-OptiScalerDir', "`"$OptiScalerDir`"", '-Owner', "`"$Owner`"", '-NoElevate')
        if ($AllShaders) { $a += '-AllShaders' }
        $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $a -Wait -PassThru
        exit $p.ExitCode
    }
    Write-Host "Taking ownership of the package folder for $Owner ..."
    & takeown.exe /f $Target /r /d y | Out-Null
    & icacls.exe $Target /grant "${Owner}:(OI)(CI)F" /t /q | Out-Null
    if (-not (Test-Writable $Target)) { throw "Still cannot write to $Target after takeown/icacls." }
}

$running = Get-Process Elgato.Studio -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Target*" }
if ($running) { throw 'Elgato Studio is running from that folder: close it (tray icon too) and run again.' }

# --- sources ---
$src = [ordered]@{
    'dxgi.dll'           = Join-Path $Repo 'deploy\reshade-winui3\dxgi_x64.dll'
    'dlss5-feed.addon64' = Join-Path $Repo 'build\dlss5-feed.addon64'
    'nvngx_dlss.dll'     = Join-Path $Repo 'deploy\shared\nvngx_dlss.dll'
    'nvngx_dlssnr.dll'   = Join-Path $Repo 'deploy\shared\nvngx_dlssnr.dll'
    'winmm.dll'          = Join-Path $OptiScalerDir 'winmm.dll'
}
$missing = $src.GetEnumerator() | Where-Object { -not (Test-Path $_.Value) }
if (-not (Test-Path (Join-Path $OptiScalerDir 'OptiScaler'))) { $missing += [pscustomobject]@{ Key = 'OptiScaler\'; Value = (Join-Path $OptiScalerDir 'OptiScaler') } }
if ($missing) { $missing | ForEach-Object { Write-Host "  missing: $($_.Key) <- $($_.Value)" -ForegroundColor Red }; throw 'Build or restore the missing files first (docs/STATUS-ELGATO-STUDIO.md).' }

Write-Host 'Pulling any live config first ...'
Invoke-Pull

# --- store: shaders + preset live here, outside the package ---
$Shaders = Join-Path $Store 'reshade-shaders\Shaders'
$Textures = Join-Path $Store 'reshade-shaders\Textures'
New-Item -ItemType Directory -Force $Shaders, $Textures | Out-Null
function Copy-IfMissing([string] $from, [string] $toDir) {
    Get-ChildItem $from -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring((Resolve-Path $from).Path.Length).TrimStart('\')
        $dst = Join-Path $toDir $rel
        if (-not (Test-Path $dst)) { New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null; Copy-Item $_.FullName $dst }
    }
}
Copy-IfMissing (Join-Path $Repo 'deploy\reshade-framework') $Shaders
Copy-IfMissing (Join-Path $Repo 'deploy\motion-vectors\lumenitefx\Shaders') $Shaders
Copy-IfMissing (Join-Path $Repo 'deploy\motion-vectors\lumenitefx\Textures') $Textures
Copy-Item (Join-Path $Repo 'shaders\DLSS5_Feed.fx') $Shaders -Force   # must match the add-on
# our own Elgato shaders (Elgato_Deband, PsychoV HDR/SDR) are tracked in the repo: always refresh
Copy-Item (Join-Path $PSScriptRoot 'shaders\*.fx') $Shaders -Force
if ($AllShaders) { & (Join-Path $Repo 'tools\Install-ReShadeShaders.ps1') -Destination (Split-Path $Shaders) }
$Preset = Join-Path $Store 'ReShadePreset.ini'
if (-not (Test-Path $Preset)) { Copy-Item (Join-Path $PSScriptRoot 'ReShadePreset.ini') $Preset }

# --- binaries ---
Write-Host 'Copying binaries ...'
foreach ($e in $src.GetEnumerator()) { Copy-Item $e.Value (Join-Path $Target $e.Key) -Force; Say $e.Key }
Copy-Item (Join-Path $OptiScalerDir 'OptiScaler') $Target -Recurse -Force; Say 'OptiScaler\'

# --- ini helpers (section-aware; OptiScaler.ini repeats key names like Enabled= per section) ---
function Set-IniValue([System.Collections.Generic.List[string]] $L, [string] $section, [string] $key, [string] $value) {
    $cur = ''; $secLine = -1; $lastInSec = -1
    for ($i = 0; $i -lt $L.Count; $i++) {
        $t = $L[$i].Trim()
        if ($t -match '^\[(.+)\]$') { $cur = $Matches[1]; if ($cur -eq $section) { $secLine = $i; $lastInSec = $i }; continue }
        if ($cur -ne $section) { continue }
        if ($t -match '^[^;#].*=') { $lastInSec = $i }   # after the last key, not the next section's comments
        if ($t -match "^$([regex]::Escape($key))\s*=") { $L[$i] = "$key=$value"; return }
    }
    if ($secLine -lt 0) { if ($L.Count -and $L[$L.Count - 1].Trim() -ne '') { $L.Add('') }; $L.Add("[$section]"); $L.Add("$key=$value"); return }
    $L.Insert($lastInSec + 1, "$key=$value")
}
function Read-Lines([string] $p) { $l = New-Object 'System.Collections.Generic.List[string]'; foreach ($x in [IO.File]::ReadAllLines($p)) { $l.Add($x) }; ,$l }

# --- ReShade.ini: saved copy or template, paths forced to the store ---
$ri = Join-Path $Saved 'ReShade.ini'
if (-not (Test-Path $ri)) { $ri = Join-Path $Repo 'deploy\templates\ReShade.ini.d3d' }
$L = Read-Lines $ri
Set-IniValue $L 'ADDON' 'AddonPath' '.\'
Set-IniValue $L 'GENERAL' 'EffectSearchPaths' "$Shaders\**"
Set-IniValue $L 'GENERAL' 'TextureSearchPaths' "$Textures\**"
Set-IniValue $L 'GENERAL' 'PresetPath' $Preset
Set-IniValue $L 'SCREENSHOT' 'SavePath' (Join-Path $Store 'screenshots')
[IO.File]::WriteAllLines((Join-Path $Target 'ReShade.ini'), $L, $Utf8)
Say "ReShade.ini (from $(Split-Path $ri -Leaf) in $(Split-Path $ri))"

# --- OptiScaler.ini: saved copy or the fork's stock ini, then the WinUI overrides ---
$oi = Join-Path $Saved 'OptiScaler.ini'
if (-not (Test-Path $oi)) { $oi = Join-Path $OptiScalerDir 'OptiScaler.ini' }
$L = Read-Lines $oi
$sec = ''
foreach ($line in [IO.File]::ReadAllLines((Join-Path $PSScriptRoot 'OptiScaler.overrides.ini'))) {
    $t = $line.Trim()
    if ($t -eq '' -or $t.StartsWith(';')) { continue }
    if ($t -match '^\[(.+)\]$') { $sec = $Matches[1]; continue }
    if ($t -match '^([^=]+?)\s*=\s*(.*)$') { Set-IniValue $L $sec $Matches[1] $Matches[2] }
}
[IO.File]::WriteAllLines((Join-Path $Target 'OptiScaler.ini'), $L, $Utf8)
Say "OptiScaler.ini (from $(Split-Path $oi -Leaf) in $(Split-Path $oi), overrides applied)"

# --- record what went where ---
$m = @("Deployed $(Get-Date -Format s) to $Target", "repo $(& git -C $Repo rev-parse --short HEAD 2>$null)")
foreach ($n in @($src.Keys)) { $m += '{0,-20} {1}' -f $n, (Get-FileHash (Join-Path $Target $n)).Hash.Substring(0, 16) }
$fc = Join-Path $OptiScalerDir 'COMMIT.txt'; if (Test-Path $fc) { $m += 'OptiScaler fork: ' + (Get-Content $fc -Raw).Trim() }
[IO.File]::WriteAllLines((Join-Path $Store 'LAST-DEPLOY.txt'), [string[]]$m, $Utf8)
Write-Host 'Done. Launch: explorer.exe shell:AppsFolder\Elgato.Studio_g54w8ztgkx496!App' -ForegroundColor Green
Write-Host "After tuning ReShade/OptiScaler, run: $PSCommandPath -Pull"
