# Run-FxLab.ps1 -- assemble a rig in build\fxlab\rig and run fxlab through the real chain:
# ReShade (deploy\reshade-dxgi) + dlss5-feed.addon64 (build\) + nvngx_dlss.dll (deploy\shared)
# + a motion-vector provider + DLSS5_Feed.fx. Needs an RTX GPU. No neural consumer: this is the
# plain DLAA pass, the part that has to leave a finished picture alone.
#
#   powershell -ExecutionPolicy Bypass -File tools\fx-testbed\Run-FxLab.ps1 -Tag base
#   ... -Tag zero -Techniques 'DLSS5_Feed@DLSS5_Feed.fx' -Provider 0     (provider absent: zero vectors)
#   ... -Tag raw  -NoReShade                                             (read-back check: mae ~0.05)
#   ... -Fx @{ MV_VALIDATE = 0 } -Cfg @{ preset = 6 }                    (shader uniforms / dlss5-feed.cfg keys)
#
#   ... -Scene 3d                                                        (the 3D courtyard: depth, HUD, true vectors)
#   ... -Scene 3d -Consumer optiscaler                                   (+ the neural pass: OptiScaler DLSS-NR fork)
#   ... -Scene 3d -Consumer optiscaler -Interactive                      (visible, 60 fps, until Esc -- to LOOK at it:
#                                                                          Space freezes the scene, Home = ReShade,
#                                                                          Insert = OptiScaler's menu)
#
# Results: build\fxlab\out\<tag>.csv and <tag>_f<frame>_<probe>.png (input | output | difference x8).
param(
    [string]$Tag = 'run',
    [string]$Shader = '',                 # DLSS5_Feed.fx to test; default: shaders\DLSS5_Feed.fx
    [int]$Provider = 3,                   # DLSS5_MV_PROVIDER
    [string]$Techniques = 'Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Feed@DLSS5_Feed.fx',
    [hashtable]$Fx = @{},                 # [DLSS5_Feed.fx] uniform overrides
    [hashtable]$Cfg = @{},                # dlss5-feed.cfg overrides
    [string]$Defines = '',                # extra preprocessor definitions for DLSS5_Feed.fx, 'A=1,B=2'
    [ValidateSet('ui', '3d')][string]$Scene = 'ui',
    [ValidateSet('', 'optiscaler')][string]$Consumer = '',   # the neural consumer: deploy\optiscaler-fork + nvngx_dlssnr.dll
    [hashtable]$Opti = @{},               # OptiScaler.ini overrides, 'Section.Key' = value
    [switch]$Interactive,                 # a visible window at 60 fps until Esc; nothing is measured
    [string]$Size = '3840x2160',
    [string]$Capture = '',
    [double]$Preroll = 14,
    [double]$Fps = -1,                    # frames a second; -1 = 120, or 60 with a neural consumer (see below); 0 = unpaced
    [string[]]$ExtraShaderDirs = @(),
    [double]$Noise = 0,                   # temporal noise on the picture, +- 8-bit steps
    [string]$Hold = '',                   # 'A,B': the scene moves for A presents, stands still for B, over and over
    [int]$Cadence = 1,                    # 2 = a 30 fps game at 60: every picture presented twice
    [switch]$Oracle,                      # exact vectors for the 3D scene: adds fxlab_Oracle.fx (use -Provider 0)
    [switch]$Hdr,                         # present HDR10 (PQ, BT.2020, 10-bit) instead of SDR
    [string]$ReShadeDll = '',             # another ReShade build to load as dxgi.dll (default: deploy\reshade-dxgi\dxgi_x64.dll)
    [int]$Fade = 0,                       # the whole picture fades down and up, once every this many frames (nothing moves for it)
    [int]$Flash = 0,                      # lightning: 45% brightness, full for three frames every this many frames
    [switch]$Mesh,                        # 3D scene: add a perforated-metal floor panel (fine repeating dots), scored apart as 3d.mesh
    [switch]$Mv,                          # measure the VECTORS against the truth instead of the picture
    [switch]$NoReShade,
    [switch]$Show,
    [switch]$Full
)
$ErrorActionPreference = 'Stop'
# With the neural model in the chain (about 8 ms at 4K on an RTX 5090) 120 frames a second is
# more than the GPU retires: the add-on's D3D12 queue falls behind, its 2 s allocator wait times
# out, the device is removed (0x887A0006) and the feed stops for the rest of the run -- which
# reads as a perfect score, because the output is then the input. 60 is what a capture feed runs at.
if ($Fps -lt 0) { $Fps = if ($Consumer -ne '') { 60 } else { 120 } }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$lab  = Join-Path $repo 'build\fxlab'
$rig  = Join-Path $lab 'rig'
$out  = Join-Path $lab 'out'
$sh   = Join-Path $rig 'reshade-shaders\Shaders'
New-Item -ItemType Directory -Force $rig, $out, $sh, (Join-Path $rig 'reshade-shaders\Textures') | Out-Null
if ($Shader -eq '') { $Shader = Join-Path $repo 'shaders\DLSS5_Feed.fx' }

function Sync($src, $dst) {
    if (-not (Test-Path $src)) { throw "missing $src" }
    if (-not (Test-Path $dst) -or (Get-Item $src).LastWriteTimeUtc -ne (Get-Item $dst).LastWriteTimeUtc -or (Get-Item $src).Length -ne (Get-Item $dst).Length) {
        Copy-Item $src $dst -Force
    }
}
Sync (Join-Path $lab 'fxlab.exe') (Join-Path $rig 'fxlab.exe')
foreach ($f in 'dxgi.dll', 'dlss5-feed.addon64', 'nvngx_dlss.dll', 'winmm.dll', 'OptiScaler.ini') { Remove-Item (Join-Path $rig $f) -ErrorAction SilentlyContinue }

# Section-aware (OptiScaler.ini repeats key names like Enabled= in several sections).
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
if (-not $NoReShade) {
    Sync $(if ($ReShadeDll -ne '') { $ReShadeDll } else { Join-Path $repo 'deploy\reshade-dxgi\dxgi_x64.dll' }) (Join-Path $rig 'dxgi.dll')
    Sync (Join-Path $repo 'build\dlss5-feed.addon64')         (Join-Path $rig 'dlss5-feed.addon64')
    Sync (Join-Path $repo 'deploy\shared\nvngx_dlss.dll')     (Join-Path $rig 'nvngx_dlss.dll')
    if ($Consumer -eq 'optiscaler') {
        # The same set, and the same ini keys, as the Elgato deploy -- minus the two that are
        # there only for a WinUI composition swap chain.
        $fork = Join-Path $repo 'deploy\optiscaler-fork'
        Sync (Join-Path $fork 'winmm.dll') (Join-Path $rig 'winmm.dll')
        Sync (Join-Path $repo 'deploy\shared\nvngx_dlssnr.dll') (Join-Path $rig 'nvngx_dlssnr.dll')
        New-Item -ItemType Directory -Force (Join-Path $rig 'OptiScaler') | Out-Null
        Get-ChildItem (Join-Path $fork 'OptiScaler') -Recurse -File | ForEach-Object {
            $rel = $_.FullName.Substring((Join-Path $fork 'OptiScaler').Length + 1)
            $dst = Join-Path (Join-Path $rig 'OptiScaler') $rel
            New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
            Sync $_.FullName $dst
        }
        $L = New-Object 'System.Collections.Generic.List[string]'
        foreach ($x in [IO.File]::ReadAllLines((Join-Path $fork 'OptiScaler.ini'))) { $L.Add($x) }
        $sec = ''
        foreach ($line in [IO.File]::ReadAllLines((Join-Path $repo 'tools\elgato\OptiScaler.overrides.ini'))) {
            $t = $line.Trim()
            if ($t -eq '' -or $t.StartsWith(';')) { continue }
            if ($t -match '^\[(.+)\]$') { $sec = $Matches[1]; continue }
            if ($t -match '^([^=]+?)\s*=\s*(.*)$' -and $Matches[1] -ne 'WrapCompositionSwapChain') { Set-IniValue $L $sec $Matches[1] $Matches[2] }
        }
        foreach ($k in $Opti.Keys) { $p = $k.Split('.', 2); Set-IniValue $L $p[0] $p[1] ([string]$Opti[$k]) }
        [IO.File]::WriteAllLines((Join-Path $rig 'OptiScaler.ini'), $L, (New-Object System.Text.UTF8Encoding($false)))
        Remove-Item (Join-Path $rig 'OptiScaler.log') -ErrorAction SilentlyContinue
    }
}
Get-ChildItem $sh -File | Remove-Item -Force
Copy-Item (Join-Path $repo 'deploy\reshade-framework\*.fxh') $sh
Copy-Item (Join-Path $repo 'deploy\motion-vectors\lumenitefx\Shaders\*') $sh -Recurse -Force
Copy-Item (Join-Path $repo 'deploy\motion-vectors\lumenitefx\Textures\*') (Join-Path $rig 'reshade-shaders\Textures') -Force
foreach ($d in $ExtraShaderDirs) { Copy-Item (Join-Path $d '*') $sh -Recurse -Force }
Copy-Item $Shader (Join-Path $sh 'DLSS5_Feed.fx') -Force
if ($Oracle) { Copy-Item (Join-Path $PSScriptRoot 'fxlab_Oracle.fx') $sh -Force }

@"
[ADDON]
AddonPath=.\

[GENERAL]
EffectSearchPaths=.\reshade-shaders\Shaders\**
TextureSearchPaths=.\reshade-shaders\Textures\**
NoDebugInfo=1
NoEffectCache=0
NoReloadOnInit=0
PerformanceMode=0
PresetPath=.\ReShadePreset.ini
SkipLoadingDisabledEffects=1

[INPUT]
KeyOverlay=$(if ($Interactive) { '36,0,0,0' } else { '0,0,0,0' })
KeyEffects=0,0,0,0
KeyScreenshot=0,0,0,0

[OVERLAY]
TutorialProgress=4
ShowClock=0
ShowFPS=0
ShowFrameTime=0
"@ | Set-Content (Join-Path $rig 'ReShade.ini') -Encoding ascii

if ($Mv) { $Techniques += ',DLSS5_Feed_Debug@DLSS5_Feed.fx'; $Fx = $Fx.Clone(); $Fx['DEBUG_VIEW'] = 10 }
$defs = "DLSS5_MV_PROVIDER=$Provider"
if ($Defines -ne '') { $defs += ",$Defines" }
$preset = @("Techniques=$Techniques", "TechniqueSorting=$Techniques", '', '[DLSS5_Feed.fx]', "PreprocessorDefinitions=$defs")
foreach ($k in $Fx.Keys) { $preset += "$k=$($Fx[$k])" }
$preset | Set-Content (Join-Path $rig 'ReShadePreset.ini') -Encoding ascii

$c = [ordered]@{ enabled = 1; mode = 2; hdr = -1; reset_every = 0; warmup_rebuild = 180; log_frames = 3; create_delay = 60
                 preset = 0; work_resolution = 100; work_upscale = 0; mv_scale_x = '1.000'; mv_scale_y = '1.000'
                 hold_strength = '0.000'; hold_tolerance = '0.040' }
foreach ($k in $Cfg.Keys) { $c[$k] = $Cfg[$k] }
($c.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) | Set-Content (Join-Path $rig 'dlss5-feed.cfg') -Encoding ascii

Remove-Item (Join-Path $rig 'dlss5-feed.log'), (Join-Path $rig 'ReShade.log') -ErrorAction SilentlyContinue
$args_ = @('--tag', $Tag, '--out', $out, '--size', $Size, '--scene', $Scene)
if ($Interactive) { $args_ += @('--fps', 60, '--loop', '--show') } else { $args_ += @('--fps', $Fps) }
if (-not $NoReShade -and -not $Interactive) { $args_ += @('--preroll', $Preroll) }
if ($Capture -ne '') { $args_ += @('--capture', $Capture) }
if ($Show -and -not $Interactive) { $args_ += '--show' }
if ($Full) { $args_ += '--full' }
if ($Mv) { $args_ += '--mv' }
if ($Mesh) { $args_ += '--mesh' }
if ($Fade -gt 0) { $args_ += @('--fade', $Fade) }
if ($Flash -gt 0) { $args_ += @('--flash', $Flash) }
if ($Hdr) { $args_ += '--hdr' }
if ($Cadence -gt 1) { $args_ += @('--cadence', $Cadence) }
if ($Oracle) { $args_ += '--oracle' }
if ($Hold -ne '') { $args_ += @('--hold', $Hold) }
if ($Noise -gt 0) { $args_ += @('--noise', $Noise) }
Push-Location $rig
try { & (Join-Path $rig 'fxlab.exe') @args_; $code = $LASTEXITCODE } finally { Pop-Location }
# With OptiScaler in the process fxlab's own lines do not reach this console (it takes stdout
# over for its log); the numbers are in the CSV either way.
if ($Consumer -ne '' -and -not $Interactive) { "results: $(Join-Path $out ($Tag + '.csv'))  (python toolsx-testbed\compare.py $Tag)" }

if (-not $NoReShade) {
    $rl = Join-Path $rig 'ReShade.log'
    if (Test-Path $rl) { Select-String -Path $rl -Pattern 'error|failed to compile|warning X' | Select-Object -First 12 | ForEach-Object { "ReShade.log: $($_.Line)" } }
    $fl = Join-Path $rig 'dlss5-feed.log'
    if (Test-Path $fl) {
        Select-String -Path $fl -Pattern 'feature ready|effects:|FAILED|failed|error|preset' | Select-Object -First 12 | ForEach-Object { "feed.log: $($_.Line)" }
        $gone = Select-String -Path $fl -Pattern 'device is removed|stopped:' | Select-Object -First 1
        if ($gone) { "feed.log: THE FEED STOPPED DURING THIS RUN -- its numbers are not a measurement: $($gone.Line)" }
    }
    else { 'feed.log: MISSING -- the add-on did not load' }
    if ($Consumer -eq 'optiscaler') {
        $ol = Join-Path $rig 'OptiScaler.log'
        if (Test-Path $ol) {
            $made = Select-String -Path $ol -Pattern 'DLSS-NR: feature created' | Select-Object -Last 1
            $time = Select-String -Path $ol -Pattern 'DLSS-NR GPU window' | Select-Object -Last 1
            if ($made) { 'OptiScaler.log: ' + ($made.Line -replace '^.*Prepare ', '') + $(if ($time) { '; ' + ($time.Line -replace '^.*EndGpuTiming ', '') }) }
            else { 'OptiScaler.log: the neural feature was NOT created' }
            Select-String -Path $ol -Pattern '\[error\]|\[critical\]|failed' | Select-Object -First 6 | ForEach-Object { "OptiScaler.log: $($_.Line)" }
        } else { 'OptiScaler.log: MISSING -- winmm.dll (OptiScaler) did not load or does not log' }
    }
}
exit $code
