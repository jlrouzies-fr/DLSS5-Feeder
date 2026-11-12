# Run-OvLab.ps1 -- assemble a rig under build\ovlab and run ovlab (a small "game" that never
# shows a window) through the real feeder chain, in any of its three shapes:
#
#   -Bits 32                    ReShade x86 + dlss5-feed.addon32 in the game, the neural tool in host64\
#   -Bits 64 -Feeder helper     ReShade x64 + dlss5-feed-helper.addon64 in the game, the tool in host64\
#   -Bits 64 -Feeder inproc     ReShade x64 + dlss5-feed.addon64 AND the tool in the game's own process
#
# Needs an RTX GPU. Nothing appears on the desktop.
#
#   powershell -ExecutionPolicy Bypass -File tools\overlay-lab\Run-OvLab.ps1 -Tag t -Consumer optiscaler
#   ... -Bits 64 -Feeder inproc -Consumer optiscaler
#   ... -Mode 2                      (the side-panel CAST of the tool's own page; 32-bit / helper only)
#   ... -Actions @('1000 key 114', '1100 shot open')    (see ovlab.cpp for the verbs)
#
# -Mode 3 (default) is the feeder's own panel, on F3 here (panel_key=114, panel_mods=0).
# Results: build\ovlab\out\<tag>\*.png, ovlab.log, and copies of the logs.
param(
    [string]$Tag = 'run',
    [ValidateSet(32, 64)][int]$Bits = 32,
    [ValidateSet('helper', 'inproc')][string]$Feeder = 'helper',   # 64-bit only; a 32-bit game always uses the helper
    [int]$Mode = 3,                       # 3 = the feeder's panel; 1 / 2 = cast_mode (texture in a corner / side panel)
    [int]$Anchor = 1,                     # 0 left, 1 right
    [string]$Size = '1280x720',
    [int]$Frames = 1500,
    [double]$Fps = 60,
    [string[]]$Actions = @(),
    [ValidateSet('renodx', 'optiscaler', 'none')][string]$Consumer = 'renodx',   # the neural tool (exactly one):
                                          # renodx = "RenoDX DLSS5 Generic" v6.1 (renodx-dlss5.addon64)
    [string]$RenoDll = '',                # another renodx-dlss5 build to use (default: v6.1 from the deploy cache)
    [hashtable]$HostIni = @{},            # keys forced into the tool's ReShade.ini before the run, 'Section.Key' = value
    [hashtable]$Opti = @{},               # OptiScaler.ini overrides, 'Section.Key' = value
    [string]$OptiDll = '',                # another OptiScaler build to load as winmm.dll (default: deploy\optiscaler-fork\winmm.dll)
    [string]$MainRepo = 'G:\AI\DLSS5-Feeder',   # where the gitignored deploy\ cache lives
    [switch]$KeepHostIni,                 # do not reset the tool's ini files: it starts from what the last run left
    [switch]$NoPlace,                     # cast A/B: leave the host's unseen window where it is
    [hashtable]$Cfg = @{}
)
$ErrorActionPreference = 'Stop'
if ($Bits -eq 32) { $Feeder = 'helper' }
$inproc = $Feeder -eq 'inproc'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$lab  = Join-Path $repo 'build\ovlab'
$rig  = Join-Path $lab $(if ($Bits -eq 32) { 'rig' } elseif ($inproc) { 'rig64' } else { 'rig64h' })
$tool = if ($inproc) { $rig } else { Join-Path $rig 'host64' }      # where the neural tool lives
$out  = Join-Path $lab "out\$Tag"
$sh   = Join-Path $rig 'reshade-shaders\Shaders'
$exe  = if ($Bits -eq 32) { 'ovlab.exe' } else { 'ovlab64.exe' }
if (Get-Process ovlab, ovlab64 -ErrorAction SilentlyContinue) { throw 'ovlab is already running' }
New-Item -ItemType Directory -Force $rig, $tool, $out, $sh | Out-Null
Get-ChildItem $out -File | Remove-Item -Force
$dep = Join-Path $MainRepo 'deploy'

# --- the game side
Copy-Item (Join-Path $lab $exe) $rig -Force
Copy-Item (Join-Path $dep $(if ($Bits -eq 32) { 'reshade-dxgi\dxgi_x86.dll' } else { 'reshade-dxgi\dxgi_x64.dll' })) (Join-Path $rig 'dxgi.dll') -Force
Remove-Item (Join-Path $rig 'dlss5-feed.addon32'), (Join-Path $rig 'dlss5-feed.addon64'), (Join-Path $rig 'dlss5-feed-helper.addon64') -ErrorAction SilentlyContinue
if ($Bits -eq 32) { Copy-Item (Join-Path $repo 'build\dlss5-feed.addon32') $rig -Force }
elseif ($inproc)  { Copy-Item (Join-Path $repo 'build\dlss5-feed.addon64') $rig -Force }
else              { Copy-Item (Join-Path $repo 'build\helper64\dlss5-feed-helper.addon64') $rig -Force }
Copy-Item (Join-Path $dep 'reshade-framework\*.fxh') $sh -Force
Copy-Item (Join-Path $repo 'shaders\DLSS5_Feed.fx') $sh -Force

# --- the helper (not for in-process)
if (-not $inproc) {
    Copy-Item (Join-Path $repo 'host\dlss5-feed-host64.exe') $tool -Force
    Copy-Item (Join-Path $dep 'reshade-dxgi\dxgi_x64.dll') (Join-Path $tool 'dxgi.dll') -Force
}

# --- the neural tool, beside whichever process runs NGX
Copy-Item (Join-Path $dep 'shared\nvngx_dlss.dll') $tool -Force
Remove-Item (Join-Path $tool 'renodx-dlss5.addon64'), (Join-Path $tool 'renodx-dlss.addon64'), (Join-Path $tool 'winmm.dll') -ErrorAction SilentlyContinue
if ($Consumer -ne 'optiscaler' -or -not $KeepHostIni) { Remove-Item (Join-Path $tool 'OptiScaler.ini') -ErrorAction SilentlyContinue }
if ($Consumer -eq 'none') { Remove-Item (Join-Path $tool 'nvngx_dlssnr.dll') -ErrorAction SilentlyContinue }
elseif (-not (Test-Path (Join-Path $tool 'nvngx_dlssnr.dll'))) { Copy-Item (Join-Path $dep 'shared\nvngx_dlssnr.dll') $tool -Force }   # 166 MB: once
if ($Consumer -eq 'renodx')    { Copy-Item $(if ($RenoDll -ne '') { $RenoDll } else { Join-Path $dep 'renodx-dlss5-v61.addon64' }) (Join-Path $tool 'renodx-dlss5.addon64') -Force }

# Section-aware and tolerant of "Key = value": OptiScaler.ini repeats key names across sections
# and the fork writes spaces round the equals sign.
function Set-IniValue([System.Collections.Generic.List[string]] $L, [string] $section, [string] $key, [string] $value) {
    $cur = ''; $secLine = -1; $lastInSec = -1
    for ($i = 0; $i -lt $L.Count; $i++) {
        $t = $L[$i].Trim()
        if ($t -match '^\[(.+)\]$') { $cur = $Matches[1]; if ($cur -eq $section) { $secLine = $i; $lastInSec = $i }; continue }
        if ($cur -ne $section) { continue }
        if ($t -match '^[^;#].*=') { $lastInSec = $i }
        if ($t -match "^$([regex]::Escape($key))\s*=") { $L[$i] = "$key=$value"; return }
    }
    if ($secLine -lt 0) { $L.Add(''); $L.Add("[$section]"); $L.Add("$key=$value"); return }
    $L.Insert($lastInSec + 1, "$key=$value")
}
if ($Consumer -eq 'optiscaler') {
    # The OptiScaler DLSS-NR fork as winmm.dll, the way the deploys carry it.
    $fork = Join-Path $dep 'optiscaler-fork'
    Copy-Item $(if ($OptiDll -ne '') { $OptiDll } else { Join-Path $fork 'winmm.dll' }) (Join-Path $tool 'winmm.dll') -Force
    robocopy (Join-Path $fork 'OptiScaler') (Join-Path $tool 'OptiScaler') /E /XO /NFL /NDL /NJH /NJS /NP | Out-Null
    if (-not (Test-Path (Join-Path $tool 'OptiScaler.ini'))) {
        $L = New-Object 'System.Collections.Generic.List[string]'
        foreach ($x in [IO.File]::ReadAllLines((Join-Path $fork 'OptiScaler.ini'))) { $L.Add($x) }
        $set = [ordered]@{ 'DlssNr.Enabled' = 'true'; 'DlssNr.FinishedPicture' = 'false'; 'Upscalers.Dx12Upscaler' = 'dlss'
                           'Log.LogToFile' = 'true'; 'Log.LogLevel' = '2'; 'Log.LogAsync' = 'false'; 'Hotfix.CheckForUpdate' = 'false' }
        foreach ($k in $Opti.Keys) { $set[$k] = [string]$Opti[$k] }
        foreach ($k in $set.Keys) { $p = $k.Split('.', 2); Set-IniValue $L $p[0] $p[1] $set[$k] }
        [IO.File]::WriteAllLines((Join-Path $tool 'OptiScaler.ini'), $L, (New-Object System.Text.UTF8Encoding($false)))
    }
    Remove-Item (Join-Path $tool 'OptiScaler.log') -ErrorAction SilentlyContinue
}

# --- ReShade's ini files. The game's: its overlay key unbound, so only the feeder's panel opens.
$gameIni = @"
[ADDON]
AddonPath=.\

[GENERAL]
EffectSearchPaths=.\reshade-shaders\Shaders\**
NoDebugInfo=1
PerformanceMode=0
PresetPath=.\ReShadePreset.ini

[INPUT]
KeyOverlay=0,0,0,0
KeyEffects=0,0,0,0
KeyScreenshot=0,0,0,0

[OVERLAY]
TutorialProgress=4
ShowClock=0
ShowFPS=0
ShowFrameTime=0
"@
# In-process the tool shares the game's ReShade.ini, so -KeepHostIni keeps that one.
if (-not ($inproc -and $KeepHostIni -and (Test-Path (Join-Path $rig 'ReShade.ini')))) { $gameIni | Set-Content (Join-Path $rig 'ReShade.ini') -Encoding ascii }
@('Techniques=DLSS5_Feed@DLSS5_Feed.fx', 'TechniqueSorting=DLSS5_Feed@DLSS5_Feed.fx', '', '[DLSS5_Feed.fx]', 'PreprocessorDefinitions=DLSS5_MV_PROVIDER=0') |
    Set-Content (Join-Path $rig 'ReShadePreset.ini') -Encoding ascii
if (-not $inproc -and -not ($KeepHostIni -and (Test-Path (Join-Path $tool 'ReShade.ini')))) {
    @"
[GENERAL]
EffectSearchPaths=.\
NoDebugInfo=1

[INPUT]
KeyOverlay=36,0,0,0
KeyEffects=0,0,0,0
KeyScreenshot=0,0,0,0

[OVERLAY]
TutorialProgress=4
ShowClock=0
ShowFPS=0
ShowFrameTime=0
"@ | Set-Content (Join-Path $tool 'ReShade.ini') -Encoding ascii
}
if ($HostIni.Count -gt 0) {
    if (-not ('OvLab.Ini' -as [type])) { Add-Type -Namespace OvLab -Name Ini -MemberDefinition '[DllImport("kernel32.dll", CharSet=CharSet.Ansi)] public static extern bool WritePrivateProfileString(string s, string k, string v, string f);' }
    foreach ($k in $HostIni.Keys) { $d = $k.LastIndexOf('.'); $q = @($k.Substring(0, $d), $k.Substring($d + 1)); [OvLab.Ini]::WritePrivateProfileString($q[0], $q[1], [string]$HostIni[$k], (Join-Path $tool 'ReShade.ini')) | Out-Null }
}

# --- dlss5-feed.cfg. F3 opens the feeder's panel (-Mode 3) or toggles the cast (-Mode 1 / 2), never both.
$panel = $Mode -eq 3
$c = [ordered]@{ enabled = 1; mode = 2; log_frames = 3; host_window = $(if ($panel) { 2 } else { 0 });   # the menu needs no helper window: none on the desktop
                 cast_key = $(if ($panel) { 0 } else { 114 }); cast_mods = 0
                 cast_scale = 100; cast_mode = $(if ($panel) { 0 } else { $Mode }); cast_anchor = $Anchor
                 panel_key = $(if ($panel) { 114 } else { 0 }); panel_mods = 0; panel_side = $Anchor }
foreach ($k in $Cfg.Keys) { $c[$k] = $Cfg[$k] }
($c.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) | Set-Content (Join-Path $rig 'dlss5-feed.cfg') -Encoding ascii

$sf = Join-Path $out 'script.txt'
$Actions | Set-Content $sf -Encoding ascii
foreach ($f in 'dlss5-feed.log', 'ReShade.log', 'OptiScaler.log', 'host64\dlss5-feed-host.log', 'host64\ReShade.log') { Remove-Item (Join-Path $rig $f) -ErrorAction SilentlyContinue }

if ($NoPlace) { $env:DLSS5_FEED_SIDE_NOPLACE = "1" } else { Remove-Item Env:DLSS5_FEED_SIDE_NOPLACE -ErrorAction SilentlyContinue }
# Run it with a deadline. A run that hangs (it has happened: a neural tool deadlocked after a
# fault) must not sit there for good, least of all when this script was started in the
# background -- nothing would ever report back. The frames, plus a minute for start-up.
$deadline = [int]($Frames / [Math]::Max($Fps, 1) + 60)
$proc = Start-Process -FilePath (Join-Path $rig $exe) -WorkingDirectory $rig -WindowStyle Hidden -PassThru `
        -ArgumentList @('--size', $Size, '--fps', $Fps, '--frames', $Frames, '--out', "`"$out`"", '--script', "`"$sf`"")
if ($proc.WaitForExit($deadline * 1000)) { $code = $proc.ExitCode }
else {
    "TIMED OUT after $deadline s: the lab game hung and was stopped. Its logs are in $rig"
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    $code = 124
}

# The helper leaves when the game does; give it a moment, then make sure nothing of this rig stays.
Start-Sleep -Milliseconds 1500
Get-Process dlss5-feed-host64 -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$rig*" } | ForEach-Object {
    "host64 (pid $($_.Id)) was still running after the game left; stopping it"; Stop-Process -Id $_.Id -Force }

Add-Type -AssemblyName System.Drawing
Get-ChildItem $out -Filter *.bmp | ForEach-Object {
    $b = [System.Drawing.Bitmap]::FromFile($_.FullName)
    $b.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    $b.Dispose(); Remove-Item $_.FullName
}
foreach ($f in 'dlss5-feed.log', 'ReShade.log') { if (Test-Path (Join-Path $rig $f)) { Copy-Item (Join-Path $rig $f) (Join-Path $out "game-$f") -Force } }
if ($inproc) { if (Test-Path (Join-Path $rig 'OptiScaler.log')) { Copy-Item (Join-Path $rig 'OptiScaler.log') (Join-Path $out 'game-OptiScaler.log') -Force } }
else { foreach ($f in 'dlss5-feed-host.log', 'ReShade.log', 'OptiScaler.log') { if (Test-Path (Join-Path $tool $f)) { Copy-Item (Join-Path $tool $f) (Join-Path $out "host-$f") -Force } } }
"exit $code; results in $out"
Get-ChildItem $out -Filter *.png | ForEach-Object { $_.Name }
exit $code
