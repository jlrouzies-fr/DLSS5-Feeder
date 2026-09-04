<#
.SYNOPSIS
    Installs DLSS5-Feeder, ReShade, a motion-vector provider and a neural consumer into a
    game folder, end to end, from one command.

.DESCRIPTION
    Point it at a game's .exe. It works out the rest:

      1. Architecture (parsed from the PE header) and render API (parsed from the import
         table, cross-checked against ReShade's own compatibility list, DXVK and dgVoodoo2).
      2. ReShade 6.8+ with add-on support: a local dxgi.dll / opengl32.dll for Direct3D and
         OpenGL, the machine-wide implicit layer plus ReShadeApps.ini entry for Vulkan.
      3. The feeder itself from the latest GitHub release: the add-on matching the game's
         bitness, the 64-bit host helper for 32-bit games, the shader, the Vulkan fallback
         layer, and the verification script.
      4. The ReShade framework headers and LumeniteFX (the recommended motion-vector provider).
      5. The neural consumer -- Deep Fried Chicken by default -- and the two NVIDIA NGX
         runtimes, in the folder where the 64-bit code runs (the game folder, or host64\).
      6. ReShade.ini and ReShadePreset.ini with the provider selected and both techniques
         enabled in the right order (merged into existing files, which are backed up first).
      7. dgVoodoo2 for Direct3D 8/9 games, configured the way the README says.
      8. The d3dcompiler_47.dll trap and the "two neural consumers" trap, defused.

    Then it runs Verify-DLSS5Feeder.ps1 on the result.

    Everything downloaded is cached (default: %LOCALAPPDATA%\DLSS5-Feeder\downloads), so a
    second install is mostly copying. Anything you already have can be handed in with
    -LocalFiles or the per-file parameters, and is then never downloaded.

    Two things need administrator rights: registering ReShade's Vulkan layer and adding an
    exe to ReShadeApps.ini (Vulkan games only), and a Windows Defender exclusion. Each is
    run as a separate elevated step, with a UAC prompt, and only when actually needed. The
    Defender exclusion is never added without asking you first: the script tries the plain
    download and extraction, and only if Defender removes Deep Fried Chicken (a known false
    positive -- it hooks NVIDIA's NGX runtime with Detours, which heuristics dislike) does it
    explain and ask.

.PARAMETER GameExe
    The game's real executable (not a launcher). A folder is accepted too, in which case the
    largest non-launcher .exe in it is picked. If omitted, the script looks in its own folder
    (drop it next to the game exe and run it), proposes what it finds, and asks before using
    it; failing that it prompts for a path.

.PARAMETER Api
    Override the render-API detection: D3D (Direct3D 10/11/12), Vulkan, OpenGL, D3D9 or
    D3D8 (both via dgVoodoo2). Default: Auto.

.PARAMETER Consumer
    Which neural consumer does the DLSS 5 work: DFC (Deep Fried Chicken) or RenoDX (Krish's
    renodx-dlss5 add-on). Both are downloaded automatically. Omitted, the script asks at the
    start; with -Yes and no choice given it takes Deep Fried Chicken.

.PARAMETER MvProvider
    DLSS5_MV_PROVIDER value: 3 (LumeniteFX Kernel, default) or 4 (LumeniteFX QuantMotion).
    Both come from the same LumeniteFX download. Other providers are not automated.

.PARAMETER Downloads
    Cache folder for downloads. Default: %LOCALAPPDATA%\DLSS5-Feeder\downloads.

.PARAMETER LocalFiles
    A folder holding any of the pieces you already have; each is used instead of a
    download when found there (matched by name): DLSS5-Feeder-*.zip,
    ReShade_Setup_*_Addon.exe, Deep-Fried-Chicken*.zip, nvngx_dlssnr.dll, nvngx_dlss.dll,
    renodx-dlss5.addon64, LumeniteFX*.zip, dgVoodoo2_*.zip, ReShade.fxh, ReShadeUI.fxh,
    DrawText.fxh.

.PARAMETER FeederZip, DfcZip, DlssNrDll, DlssDll, RenoDxAddon, ReShadeSetup, LumeniteZip, DgVoodooZip
    Explicit path or URL for one piece, overriding both -LocalFiles and the defaults.

.PARAMETER Prerelease
    Take the newest GitHub release even if it is marked pre-release.

.PARAMETER DgVoodooWatermark
    Leave dgVoodoo2's corner watermark on (it is turned off by default). Useful the first
    time, to prove dgVoodoo is active at all.

.PARAMETER Force
    Overwrite an existing, new-enough ReShade DLL, and replace (after backing up) rather
    than merge existing ReShade.ini / ReShadePreset.ini files.

.PARAMETER Yes
    Answer yes to every confirmation (Defender exclusion, disabling conflicting files).
    For unattended use only.

.PARAMETER NoElevate
    Never show a UAC prompt. Steps that need one are printed as manual instructions.

.PARAMETER NoVerify
    Skip the closing Verify-DLSS5Feeder.ps1 run.

.PARAMETER NoPause
    Skip the "Press Enter to exit" pause at the end.

.EXAMPLE
    .\Install-DLSS5Feeder.ps1 "G:\Games\Dusk\Dusk.exe"

.EXAMPLE
    .\Install-DLSS5Feeder.ps1 "E:\SteamLibrary\steamapps\common\DOOM\DOOMx64vk.exe" -Api Vulkan

.EXAMPLE
    .\Install-DLSS5Feeder.ps1 "D:\Games\Fable Anniversary\Binaries\Win32\Fable Anniversary.exe" -LocalFiles D:\Downloads

.NOTES
    Windows PowerShell 5.1 compatible. Needs an internet connection for anything not cached
    or supplied. If Windows refuses to run it, use:
      powershell -ExecutionPolicy Bypass -File .\Install-DLSS5Feeder.ps1 "<path to game.exe>"
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string] $GameExe,

    [ValidateSet('Auto', 'D3D', 'Vulkan', 'OpenGL', 'D3D9', 'D3D8')]
    [string] $Api = 'Auto',

    [ValidateSet('Ask', 'DFC', 'RenoDX')]
    [string] $Consumer = 'Ask',

    [ValidateSet(3, 4)]
    [int] $MvProvider = 3,

    [string] $Downloads,
    [string] $LocalFiles,

    [string] $FeederZip,
    [string] $DfcZip,
    [string] $DlssNrDll,
    [string] $DlssDll,
    [string] $RenoDxAddon,
    [string] $ReShadeSetup,
    [string] $LumeniteZip,
    [string] $DgVoodooZip,

    [switch] $Prerelease,
    [switch] $DgVoodooWatermark,
    [switch] $Force,
    [switch] $Yes,
    [switch] $NoElevate,
    [switch] $NoVerify,
    [switch] $NoPause
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# ---------------------------------------------------------------------------------------
# Where things come from. Edit here when a link moves.
#
# The three Discord CDN links carry an "ex=" expiry (hex Unix time) and stop working after
# it; the script decodes it and says so rather than reporting a bare 403/404. Fresh links
# are in the Discord servers the README points at.
# ---------------------------------------------------------------------------------------

$Sources = @{
    FeederReleases  = 'https://api.github.com/repos/jlrouzies-fr/DLSS5-Feeder/releases'
    ReShadeHome     = 'https://reshade.me'
    ReShadeFallback = 'https://reshade.me/downloads/ReShade_Setup_6.8.0_Addon.exe'
    Headers         = 'https://raw.githubusercontent.com/crosire/reshade-shaders/slim/Shaders/'
    CompatIni       = 'https://raw.githubusercontent.com/crosire/reshade-shaders/list/Compatibility.ini'
    Lumenite        = 'https://codeload.github.com/umar-afzaal/LumeniteFX/zip/refs/heads/mainline'
    DgVoodoo        = 'https://api.github.com/repos/dege-diosg/dgVoodoo2/releases/latest'
    Dfc             = 'https://cdn.discordapp.com/attachments/1543936250657120366/1544601537844879410/Deep-Fried-Chicken-v1.4.8-alpha.zip?ex=6a99c287&is=6a987107&hm=9460267dc5be8024653c5d1feb6fff6f5d00f55bf2ab0262ffcfd285e1b7d143&'
    DlssNr          = 'https://cdn.discordapp.com/attachments/1543976771920330884/1543982044797866107/nvngx_dlssnr.dll?ex=6a9a2495&is=6a98d315&hm=a0a12bd2e4d7ae4c7e915a21e1570c594af0e2cf2e15195d9dbf8a693f45ca99&'
    Dlss            = 'https://cdn.discordapp.com/attachments/1543348014691651676/1544918856697643089/nvngx_dlss.dll?ex=6a9a414e&is=6a98efce&hm=17a6973ef6de0d211b7b3fe00362d851685156e73aab3e38670e00563332b22a&'
    RenoDxDlss5     = 'https://cdn.discordapp.com/attachments/1542647972695904317/1544338777399365762/renodx-dlss5.addon64?ex=6a9a1f50&is=6a98cdd0&hm=2a695add57b27c6d7fd1ad2a70e2d3c4f49586a3d5c30f84cc33be3513d41de8&'
    DfcDiscord      = 'https://discord.gg/g2v2XGqvR'
    RenoDxDiscord   = 'https://discord.com/invite/renodx'
}

$ReShadeMinVersion = '6.8'

# ---------------------------------------------------------------------------------------
# Output plumbing (same look as Verify-DLSS5Feeder.ps1)
# ---------------------------------------------------------------------------------------

$script:CountDone = 0
$script:CountWarn = 0
$script:CountFail = 0
$script:Manual    = New-Object System.Collections.ArrayList   # steps the user must do
$script:Changed   = New-Object System.Collections.ArrayList   # files written, for Unblock-File

$script:UseColour = $true
try {
    if ($null -eq $Host -or $null -eq $Host.UI -or $null -eq $Host.UI.RawUI) { $script:UseColour = $false }
    else { $null = $Host.UI.RawUI.ForegroundColor }
}
catch { $script:UseColour = $false }

function Write-Chunk
{
    param([string] $Text, [string] $Colour, [switch] $NoNewline)
    try {
        if ($script:UseColour -and $Colour) { Write-Host $Text -ForegroundColor $Colour -NoNewline:$NoNewline }
        else { Write-Host $Text -NoNewline:$NoNewline }
    }
    catch {
        $script:UseColour = $false
        Write-Host $Text -NoNewline:$NoNewline
    }
}

function Write-Banner
{
    $box = 'DarkGray'
    $w   = 68
    Write-Host ''
    Write-Chunk ('  ' + [char]0x2554 + ([string][char]0x2550) * $w + [char]0x2557) $box

    # The logo's mark: three rounded squares stacked back-to-front, offset down and left.
    $blk = [string][char]0x2588   # full block
    $upr = [string][char]0x2580   # upper half block
    $lwr = [string][char]0x2584   # lower half block

    $rows = @(
        @{ Mark = '      ' + ($lwr * 5);          Shade = 'DarkGreen'; Text = ''; TextColour = '' },
        @{ Mark = '    ' + ($lwr * 2) + ($blk * 5); Shade = 'Green';   Text = '   DLSS5-Feeder installer'; TextColour = 'White' },
        @{ Mark = '   ' + ($blk * 6) + ($upr * 2); Shade = 'Green';    Text = '   ReShade, feeder, motion vectors, neural consumer'; TextColour = 'DarkGray' },
        @{ Mark = '   ' + ($upr * 6);             Shade = 'DarkGreen'; Text = ''; TextColour = '' }
    )

    foreach ($r in $rows) {
        Write-Chunk ('  ' + [char]0x2551) $box -NoNewline
        Write-Chunk $r.Mark $r.Shade -NoNewline
        if ($r.Text) { Write-Chunk $r.Text $r.TextColour -NoNewline }
        $used = $r.Mark.Length + $r.Text.Length
        if ($used -lt $w) { Write-Chunk ((' ') * ($w - $used)) $null -NoNewline }
        Write-Chunk ([string][char]0x2551) $box
    }

    Write-Chunk ('  ' + [char]0x255A + ([string][char]0x2550) * $w + [char]0x255D) $box
    Write-Host ''
}

function Write-Section
{
    param([string] $Title)
    Write-Host ''
    Write-Chunk ('  ' + [char]0x2500 + [char]0x2500 + ' ') 'Green' -NoNewline
    Write-Chunk $Title 'White' -NoNewline
    $pad = 62 - $Title.Length
    if ($pad -lt 1) { $pad = 1 }
    Write-Chunk (' ' + ([string][char]0x2500) * $pad) 'Green'
}

# $Status: Done (something was installed/written), Ok (already right, nothing to do),
# Skip (not applicable), Warn, Fail, Info.
function Report
{
    param(
        [ValidateSet('Done', 'Ok', 'Skip', 'Warn', 'Fail', 'Info')]
        [string] $Status,
        [string] $Text,
        [string] $Detail,
        [string] $Manual
    )

    switch ($Status) {
        'Done' { $glyph = '[DONE]'; $colour = 'Green';    $script:CountDone++ }
        'Ok'   { $glyph = '[ OK ]'; $colour = 'Green' }
        'Skip' { $glyph = '[ -- ]'; $colour = 'DarkGray' }
        'Warn' { $glyph = '[WARN]'; $colour = 'Yellow';   $script:CountWarn++ }
        'Fail' { $glyph = '[FAIL]'; $colour = 'Red';      $script:CountFail++ }
        'Info' { $glyph = '[ .. ]'; $colour = 'DarkGray' }
    }

    if ($Manual) { $null = $script:Manual.Add($Manual) }

    Write-Chunk ('  ' + $glyph + ' ') $colour -NoNewline
    if ($Status -eq 'Skip' -or $Status -eq 'Info') { Write-Chunk $Text 'DarkGray' } else { Write-Host $Text }
    if ($Detail) {
        foreach ($line in ($Detail -split "`n")) {
            if ($line.Trim()) { Write-Chunk ('         ' + $line.Trim()) 'DarkGray' }
        }
    }
    if ($Manual) {
        $first = $true
        foreach ($line in ($Manual -split "`n")) {
            if (-not $line.Trim()) { continue }
            if ($first) { Write-Chunk ('         ' + [char]0x2192 + ' ' + $line.Trim()) 'DarkYellow'; $first = $false }
            else { Write-Chunk ('           ' + $line.Trim()) 'DarkYellow' }
        }
    }
}

function Exit-Installer
{
    param([int] $Code)
    if (-not $NoPause) {
        Write-Host ''
        try { [void](Read-Host '  Press Enter to exit') } catch { }
    }
    exit $Code
}

function Stop-Install
{
    param([string] $Text, [string] $Detail, [string] $Manual)
    Report -Status 'Fail' -Text $Text -Detail $Detail -Manual $Manual
    Write-Host ''
    Write-Chunk '  Stopped: the install cannot continue from here.' 'Red'
    Exit-Installer 1
}

function Confirm-Step
{
    param([string] $Question)
    if ($Yes) { return $true }
    Write-Host ''
    Write-Chunk ('  ' + $Question + ' [y/N] ') 'Cyan' -NoNewline
    try { $a = Read-Host } catch { return $false }
    return ($a -match '^(?i)y(es)?$')
}

# ---------------------------------------------------------------------------------------
# Small, defensive helpers
# ---------------------------------------------------------------------------------------

function Join-Safe
{
    param([string] $Parent, [string] $Child)
    try { return [IO.Path]::Combine($Parent, $Child) } catch { return $null }
}

function Test-FileHere
{
    param([string] $Path)
    if (-not $Path) { return $false }
    try { return (Test-Path -LiteralPath $Path -PathType Leaf) } catch { return $false }
}

function Test-DirHere
{
    param([string] $Path)
    if (-not $Path) { return $false }
    try { return (Test-Path -LiteralPath $Path -PathType Container) } catch { return $false }
}

function New-DirSafe
{
    param([string] $Path)
    if (-not (Test-DirHere $Path)) { $null = New-Item -ItemType Directory -Path $Path -Force }
}

function Find-FileIn
{
    param([string] $Dir, [string] $Name)
    if (-not (Test-DirHere $Dir)) { return $null }
    $p = Join-Safe $Dir $Name
    if (Test-FileHere $p) { return $p }
    try {
        $hit = Get-ChildItem -LiteralPath $Dir -File -Filter $Name -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    catch { }
    return $null
}

function Find-FileUnder
{
    param([string] $Dir, [string] $Name)
    if (-not (Test-DirHere $Dir)) { return $null }
    try {
        $hit = Get-ChildItem -LiteralPath $Dir -File -Filter $Name -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    catch { }
    return $null
}

function Get-FileVersionSafe
{
    param([string] $Path)
    if (-not (Test-FileHere $Path)) { return $null }
    try {
        $vi = (Get-Item -LiteralPath $Path -ErrorAction Stop).VersionInfo
        if ($vi -and $vi.FileVersion) { return ($vi.FileVersion.Trim() -replace '\s*,\s*', '.') }
    }
    catch { }
    return $null
}

function Get-ProductNameSafe
{
    param([string] $Path)
    if (-not (Test-FileHere $Path)) { return $null }
    try {
        $vi = (Get-Item -LiteralPath $Path -ErrorAction Stop).VersionInfo
        $bits = @()
        if ($vi.ProductName)     { $bits += $vi.ProductName }
        if ($vi.FileDescription) { $bits += $vi.FileDescription }
        if ($vi.InternalName)    { $bits += $vi.InternalName }
        return ($bits -join ' | ')
    }
    catch { }
    return $null
}

function Format-Size
{
    param([long] $Bytes)
    if ($Bytes -ge 1073741824) { return ('{0:N1} GB' -f ($Bytes / 1073741824)) }
    if ($Bytes -ge 1048576)    { return ('{0:N1} MB' -f ($Bytes / 1048576)) }
    if ($Bytes -ge 1024)       { return ('{0:N0} KB' -f ($Bytes / 1024)) }
    return ('{0} B' -f $Bytes)
}

# Scan a binary for an ASCII marker. Capped: never slurp a 160 MB NGX runtime.
function Get-BinaryMarker
{
    param([string] $Path, [string] $Pattern, [int] $MaxBytes = 33554432)
    if (-not (Test-FileHere $Path)) { return $null }
    try {
        $fi = Get-Item -LiteralPath $Path -ErrorAction Stop
        if ($fi.Length -gt $MaxBytes -or $fi.Length -eq 0) { return $null }
        $bytes = [IO.File]::ReadAllBytes($Path)
        $m = [regex]::Match([Text.Encoding]::ASCII.GetString($bytes), $Pattern)
        if ($m.Success) {
            if ($m.Groups.Count -gt 1 -and $m.Groups[1].Success) { return $m.Groups[1].Value }
            return $m.Value
        }
    }
    catch { }
    return $null
}

function Get-Sha256
{
    param([string] $Path)
    try { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash.ToUpperInvariant() } catch { return $null }
}

function Copy-Tracked
{
    # Copy with the destination folder created, and remember the file for Unblock-File.
    param([string] $From, [string] $To)
    New-DirSafe ([IO.Path]::GetDirectoryName($To))
    Copy-Item -LiteralPath $From -Destination $To -Force
    $null = $script:Changed.Add($To)
}

function Write-TextTracked
{
    param([string] $Path, [string] $Text)
    New-DirSafe ([IO.Path]::GetDirectoryName($Path))
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding $false))
    $null = $script:Changed.Add($Path)
}

function Backup-File
{
    param([string] $Path)
    if (-not (Test-FileHere $Path)) { return $null }
    $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
    $bak = $Path + '.bak-' + $stamp
    Copy-Item -LiteralPath $Path -Destination $bak -Force
    return $bak
}

function Read-TextSafe
{
    param([string] $Path)
    if (-not (Test-FileHere $Path)) { return $null }
    try {
        $t = [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)
        if ($t.Length -gt 0 -and [int]$t[0] -eq 0xFEFF) { $t = $t.Substring(1) }
        return $t
    }
    catch { return $null }
}

# ---------------------------------------------------------------------------------------
# PE parsing: architecture and the static import table (what ReShade's setup does to pick
# the API). Works on a file a running game holds open.
# ---------------------------------------------------------------------------------------

function Get-PeInfo
{
    param([string] $Path)
    if (-not (Test-FileHere $Path)) { return $null }

    $fs = $null
    $br = $null
    try {
        $fs = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        if ($fs.Length -lt 0x40) { return $null }
        $br = New-Object IO.BinaryReader($fs)

        if ($br.ReadUInt16() -ne 0x5A4D) { return $null }
        $fs.Position = 0x3C
        $peOff = $br.ReadInt32()
        if ($peOff -le 0 -or ($peOff + 24) -ge $fs.Length) { return $null }

        $fs.Position = $peOff
        if ($br.ReadUInt32() -ne 0x00004550) { return $null }

        $machine   = $br.ReadUInt16()
        $nSections = $br.ReadUInt16()
        $null      = $br.ReadUInt32()   # timestamp
        $null      = $br.ReadUInt32()   # symbol table
        $null      = $br.ReadUInt32()   # symbol count
        $optSize   = $br.ReadUInt16()
        $null      = $br.ReadUInt16()   # characteristics

        switch ($machine) {
            0x014C  { $bits = 32; $arch = 'x86' }
            0x8664  { $bits = 64; $arch = 'x64' }
            0xAA64  { $bits = 64; $arch = 'ARM64' }
            0x01C4  { $bits = 32; $arch = 'ARM' }
            default { $bits = 0;  $arch = ('unknown machine 0x{0:X4}' -f $machine) }
        }

        $imports = @()
        $subsystemVersion = $null
        try {
            $optOff = $peOff + 24
            $fs.Position = $optOff
            $magic = $br.ReadUInt16()
            if ($magic -eq 0x10B) { $ddOff = $optOff + 96 } elseif ($magic -eq 0x20B) { $ddOff = $optOff + 112 } else { $ddOff = 0 }

            if ($ddOff -gt 0) {
                $fs.Position = $optOff + 40
                $subsystemVersion = '{0}.{1}' -f $br.ReadUInt16(), $br.ReadUInt16()

                $fs.Position = $ddOff + 8     # data directory [1] = imports
                $impRva  = $br.ReadUInt32()
                $impSize = $br.ReadUInt32()

                $sections = @()
                $secOff = $optOff + $optSize
                for ($i = 0; $i -lt $nSections; $i++) {
                    $fs.Position = $secOff + $i * 40 + 8
                    $vsize = $br.ReadUInt32(); $vaddr = $br.ReadUInt32(); $rsize = $br.ReadUInt32(); $rptr = $br.ReadUInt32()
                    $span = [math]::Max($vsize, $rsize)
                    $sections += New-Object psobject -Property @{ VA = $vaddr; Span = $span; Raw = $rptr }
                }

                $rvaToOff = {
                    param([uint32] $rva)
                    foreach ($s in $sections) {
                        if ($rva -ge $s.VA -and $rva -lt ($s.VA + $s.Span)) { return [long]($rva - $s.VA + $s.Raw) }
                    }
                    return -1
                }

                if ($impRva -gt 0 -and $impSize -gt 0) {
                    $descOff = & $rvaToOff $impRva
                    $n = 0
                    while ($descOff -ge 0 -and ($descOff + 20) -lt $fs.Length -and $n -lt 512) {
                        $fs.Position = $descOff + 12
                        $nameRva = $br.ReadUInt32()
                        if ($nameRva -eq 0) { break }
                        $nameOff = & $rvaToOff $nameRva
                        if ($nameOff -ge 0 -and $nameOff -lt $fs.Length) {
                            $fs.Position = $nameOff
                            $sb = New-Object Text.StringBuilder
                            for ($k = 0; $k -lt 260; $k++) {
                                $b = $fs.ReadByte()
                                if ($b -le 0) { break }
                                $null = $sb.Append([char]$b)
                            }
                            if ($sb.Length -gt 0) { $imports += $sb.ToString() }
                        }
                        $descOff += 20
                        $n++
                    }
                }
            }
        }
        catch { }

        return New-Object psobject -Property @{
            Bits = $bits; Arch = $arch; Imports = $imports; SubsystemVersion = $subsystemVersion
        }
    }
    catch { return $null }
    finally {
        if ($br) { try { $br.Close() } catch { } }
        if ($fs) { try { $fs.Dispose() } catch { } }
    }
}

# Counts occurrences of the graphics-API DLL names (ASCII and UTF-16) inside a binary.
# Only used when the import table says nothing. Capped at 256 MB.
function Get-ApiStringHints
{
    param([string] $Path)
    $hits = @{}
    try {
        $fi = Get-Item -LiteralPath $Path -ErrorAction Stop
        if ($fi.Length -gt 268435456 -or $fi.Length -eq 0) { return $hits }
        $bytes = [IO.File]::ReadAllBytes($Path)
        $ascii = [Text.Encoding]::ASCII.GetString($bytes)
        $wide  = [Text.Encoding]::Unicode.GetString($bytes)
        # d3d10_1.dll earns its place: a Direct3D 10 game imports THAT, not d3d10.dll, and
        # without it here a title like Devil May Cry 4 SE -- which also imports d3d9.dll for
        # its D3DPERF markers -- was detected as Direct3D 9 and sent down the dgVoodoo2 route,
        # which implements DirectX 1-9 and cannot touch Direct3D 10.
        foreach ($n in @('vulkan-1.dll', 'dxgi.dll', 'd3d12.dll', 'd3d11.dll', 'd3d10_1.dll', 'd3d10.dll', 'd3d10core.dll', 'd3d9.dll', 'd3d8.dll', 'opengl32.dll')) {
            $re = '(?i)(?<![\w.])' + [regex]::Escape($n)
            $c = [regex]::Matches($ascii, $re).Count + [regex]::Matches($wide, $re).Count
            if ($c -gt 0) { $hits[$n] = $c }
        }
    }
    catch { }
    return $hits
}

# ---------------------------------------------------------------------------------------
# Downloads
# ---------------------------------------------------------------------------------------

try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 -bor 0x3000 } catch { }

function New-WebClient
{
    $wc = New-Object Net.WebClient
    $wc.Headers.Add('User-Agent', 'DLSS5-Feeder-Installer (PowerShell)')
    return $wc
}

function Get-WebString
{
    param([string] $Url)
    $wc = New-WebClient
    try { return $wc.DownloadString($Url) } catch { return $null } finally { $wc.Dispose() }
}

# Decodes the "ex=" expiry on a Discord CDN link. Returns a DateTime or $null.
function Get-DiscordExpiry
{
    param([string] $Url)
    $m = [regex]::Match($Url, '[?&]ex=([0-9a-fA-F]{6,10})')
    if (-not $m.Success) { return $null }
    try {
        $secs = [Convert]::ToInt64($m.Groups[1].Value, 16)
        return ([DateTimeOffset]::FromUnixTimeSeconds($secs)).LocalDateTime
    }
    catch { return $null }
}

# Downloads $Url to $Dest unless $Dest already exists. Returns $true on success. Never throws.
function Get-Download
{
    param([string] $Url, [string] $Dest, [string] $Label)

    if (Test-FileHere $Dest) {
        Report -Status 'Ok' -Text ($Label + ': cached') -Detail $Dest
        return $true
    }

    if ($Url -match '(?i)^https?://') {
        $exp = Get-DiscordExpiry $Url
        if ($exp -and $exp -lt (Get-Date)) {
            Report -Status 'Fail' -Text ($Label + ': the download link expired on ' + $exp.ToString('yyyy-MM-dd HH:mm') + '.') `
                   -Detail 'Discord CDN links carry an expiry. Get a fresh link from the Discord server the README points at, and pass it (or the downloaded file) with the matching parameter.'
            return $false
        }
    }
    elseif (Test-FileHere $Url) {
        # A local path was given where a URL was expected: just copy it.
        try { Copy-Tracked -From $Url -To $Dest; Report -Status 'Done' -Text ($Label + ': copied from ' + $Url); return $true }
        catch { Report -Status 'Fail' -Text ($Label + ': cannot copy ' + $Url) -Detail $_.Exception.Message; return $false }
    }
    else {
        Report -Status 'Fail' -Text ($Label + ': not a URL and not an existing file: ' + $Url)
        return $false
    }

    New-DirSafe ([IO.Path]::GetDirectoryName($Dest))
    $tmp = $Dest + '.part'
    Write-Chunk ('  [ .. ] ' + $Label + ': downloading ...') 'DarkGray'
    $wc = New-WebClient
    try {
        $wc.DownloadFile($Url, $tmp)
        if (-not (Test-FileHere $tmp)) { throw 'the download produced no file' }
        $len = (Get-Item -LiteralPath $tmp).Length
        if ($len -lt 64) { throw 'the download is empty' }
        Move-Item -LiteralPath $tmp -Destination $Dest -Force
        $null = $script:Changed.Add($Dest)
        Report -Status 'Done' -Text ($Label + ': downloaded (' + (Format-Size $len) + ')') -Detail $Dest
        return $true
    }
    catch {
        $msg = $_.Exception.Message
        if ($_.Exception.InnerException) { $msg = $_.Exception.InnerException.Message }
        try { if (Test-FileHere $tmp) { Remove-Item -LiteralPath $tmp -Force } } catch { }
        $d = 'From: ' + $Url + "`n" + $msg
        if ($msg -match '(?i)virus|potentially unwanted|0x800700E1') { $d += "`nWindows Defender blocked the download itself." }
        Report -Status 'Fail' -Text ($Label + ': download failed.') -Detail $d
        return $false
    }
    finally { $wc.Dispose() }
}

# Resolve where a piece comes from: explicit parameter (path or URL), -LocalFiles match,
# then the default URL. Returns a path in the cache, or $null.
function Resolve-Piece
{
    param([string] $Label, [string] $Explicit, [string] $LocalPattern, [string] $DefaultUrl, [string] $CacheName)

    $dest = Join-Safe $script:Cache $CacheName

    if ($Explicit) {
        if (Test-FileHere $Explicit) { return $Explicit }
        if (Get-Download -Url $Explicit -Dest $dest -Label $Label) { return $dest }
        return $null
    }
    if ($LocalFiles -and $LocalPattern) {
        try {
            $hit = Get-ChildItem -LiteralPath $LocalFiles -File -Filter $LocalPattern -ErrorAction SilentlyContinue |
                   Sort-Object LastWriteTime -Descending | Select-Object -First 1
            if ($hit) { Report -Status 'Ok' -Text ($Label + ': using ' + $hit.FullName); return $hit.FullName }
        }
        catch { }
    }
    if (-not $DefaultUrl) { return $null }
    if (Get-Download -Url $DefaultUrl -Dest $dest -Label $Label) { return $dest }
    return $null
}

# ---------------------------------------------------------------------------------------
# Zip helpers. ReShade's setup exe is a zip appended to an executable, which .NET refuses
# to open directly, so Open-Zip finds the archive start the way the setup itself does.
# ---------------------------------------------------------------------------------------

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Open-Zip
{
    param([string] $Path)
    $bytes = [IO.File]::ReadAllBytes($Path)
    $start = -1
    if ($bytes.Length -ge 4 -and $bytes[0] -eq 0x50 -and $bytes[1] -eq 0x4B -and $bytes[2] -eq 3 -and $bytes[3] -eq 4) {
        $start = 0
    }
    else {
        for ($i = 0; $i -le $bytes.Length - 30; $i += 512) {
            if ($bytes[$i] -eq 0x50 -and $bytes[$i + 1] -eq 0x4B -and $bytes[$i + 2] -eq 3 -and $bytes[$i + 3] -eq 4) {
                $nonZero = $false
                for ($k = 4; $k -lt 30; $k++) { if ($bytes[$i + $k] -ne 0) { $nonZero = $true; break } }
                if ($nonZero) { $start = $i; break }
            }
        }
    }
    if ($start -lt 0) { throw ('no zip archive found in ' + $Path) }
    $ms = New-Object IO.MemoryStream(, $bytes)
    $ms.Position = $start
    if ($start -gt 0) {
        $rest = New-Object IO.MemoryStream
        $ms.CopyTo($rest)
        $ms.Dispose()
        $ms = $rest
        $ms.Position = 0
    }
    return New-Object IO.Compression.ZipArchive($ms, [IO.Compression.ZipArchiveMode]::Read)
}

function Find-ZipEntry
{
    param($Zip, [string] $Pattern)   # regex on the full entry name
    foreach ($e in $Zip.Entries) { if (($e.FullName -replace '\\', '/') -match $Pattern) { return $e } }
    return $null
}

function Expand-ZipEntry
{
    param($Entry, [string] $To)
    New-DirSafe ([IO.Path]::GetDirectoryName($To))
    $in = $Entry.Open()
    try {
        $out = New-Object IO.FileStream($To, [IO.FileMode]::Create, [IO.FileAccess]::Write)
        try { $in.CopyTo($out) } finally { $out.Dispose() }
    }
    finally { $in.Dispose() }
    $null = $script:Changed.Add($To)
}

function Read-ZipText
{
    param($Entry)
    $in = $Entry.Open()
    try { $sr = New-Object IO.StreamReader($in); try { return $sr.ReadToEnd() } finally { $sr.Dispose() } }
    finally { $in.Dispose() }
}

# ---------------------------------------------------------------------------------------
# Ini editing that keeps everything else in the file intact.
# ---------------------------------------------------------------------------------------

function Set-IniKey
{
    # $Section '' means the key lives before the first [section] (ReShadePreset's Techniques=).
    param([string] $Text, [string] $Section, [string] $Key, [string] $Value)

    if ($null -eq $Text) { $Text = '' }
    $nl = "`r`n"
    $lines = New-Object System.Collections.ArrayList
    foreach ($l in ($Text -split "`r?`n")) { $null = $lines.Add($l) }
    # Drop one trailing empty line produced by a final newline, so we can append cleanly.
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }

    $cur = ''
    $secStart = -1
    $secEnd = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^\[(.+)\]$') {
            if ($cur -ieq $Section -and $secStart -ge 0 -and $secEnd -lt 0) { $secEnd = $i }
            $cur = $Matches[1]
            if ($cur -ieq $Section) { $secStart = $i + 1 }
            continue
        }
        if ($cur -ieq $Section -and $t -match ('(?i)^' + [regex]::Escape($Key) + '\s*=')) {
            $lines[$i] = $Key + '=' + $Value
            return (($lines -join $nl) + $nl)
        }
    }
    if ($Section -eq '' -and $secStart -lt 0) { $secStart = 0; $secEnd = 0; for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Trim() -match '^\[') { break }; $secEnd = $i + 1 } }
    if ($secStart -ge 0) {
        if ($secEnd -lt 0) { $secEnd = $lines.Count }
        # Insert after the last non-blank line of the section.
        $at = $secEnd
        while ($at -gt $secStart -and $lines[$at - 1].Trim() -eq '') { $at-- }
        $lines.Insert($at, ($Key + '=' + $Value))
    }
    else {
        if ($lines.Count -gt 0) { $null = $lines.Add('') }
        $null = $lines.Add('[' + $Section + ']')
        $null = $lines.Add($Key + '=' + $Value)
    }
    return (($lines -join $nl) + $nl)
}

function Get-IniKey
{
    param([string] $Text, [string] $Section, [string] $Key)
    if ($null -eq $Text) { return $null }
    $cur = ''
    foreach ($l in ($Text -split "`r?`n")) {
        $t = $l.Trim()
        if ($t -match '^\[(.+)\]$') { $cur = $Matches[1]; continue }
        if ($cur -ieq $Section -and $t -match ('(?i)^' + [regex]::Escape($Key) + '\s*=\s*(.*)$')) { return $Matches[1] }
    }
    return $null
}

# dgVoodoo.conf keeps its values column-aligned; replace only the value part.
function Set-ConfValue
{
    param([string] $Text, [string] $Section, [string] $Key, [string] $Value)
    $lines = $Text -split "`r?`n"
    $cur = ''
    $done = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^\[(.+)\]$') { $cur = $Matches[1]; continue }
        if (-not $done -and $cur -ieq $Section -and $lines[$i] -match ('(?i)^(\s*' + [regex]::Escape($Key) + '\s*=\s*)')) {
            $lines[$i] = $Matches[1] + $Value
            $done = $true
        }
    }
    if (-not $done) { return (Set-IniKey -Text $Text -Section $Section -Key $Key -Value $Value) }
    return ($lines -join "`r`n")
}

# ---------------------------------------------------------------------------------------
# Elevation: one temporary script, one UAC prompt, for a named list of tasks.
# ---------------------------------------------------------------------------------------

function Test-Elevated
{
    try {
        $id = [Security.Principal.WindowsIdentity]::GetCurrent()
        return (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    }
    catch { return $false }
}

# Returns 'ok', 'declined' or 'failed'. $Script is PowerShell text; it runs with
# $ErrorActionPreference = 'Stop' and exits 0 on success.
function Invoke-Elevated
{
    param([string] $Script, [string] $What)

    $body = "`$ErrorActionPreference = 'Stop'`r`ntry {`r`n" + $Script + "`r`n  exit 0`r`n}`r`ncatch {`r`n  Write-Host `$_.Exception.Message`r`n  Start-Sleep -Seconds 4`r`n  exit 1`r`n}`r`n"
    $tmp = Join-Safe $script:Cache ('elevated-' + (Get-Date).ToString('yyyyMMdd-HHmmss-fff') + '.ps1')
    [IO.File]::WriteAllText($tmp, $body, (New-Object Text.UTF8Encoding $true))

    if (Test-Elevated) {
        try { & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $tmp; $code = $LASTEXITCODE }
        catch { $code = 1 }
    }
    else {
        Write-Chunk ('  [ .. ] Asking for administrator rights: ' + $What) 'Cyan'
        try {
            $p = Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -PassThru `
                     -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $tmp + '"'))
            $code = $p.ExitCode
        }
        catch {
            try { Remove-Item -LiteralPath $tmp -Force } catch { }
            return 'declined'
        }
    }
    try { Remove-Item -LiteralPath $tmp -Force } catch { }
    if ($code -eq 0) { return 'ok' }
    return 'failed'
}

function ConvertTo-PsLiteral
{
    param([string] $S)
    return ("'" + ($S -replace "'", "''") + "'")
}

# ---------------------------------------------------------------------------------------
# Windows Defender: detect a removal, and ask before excluding.
# ---------------------------------------------------------------------------------------

function Get-DefenderDetection
{
    # A detection in the last 15 minutes naming $Path (or the Chicken zip) -- or $null.
    param([string] $Path)
    try {
        $since = (Get-Date).AddMinutes(-15)
        $all = Get-MpThreatDetection -ErrorAction Stop
        foreach ($d in $all) {
            if ($d.InitialDetectionTime -lt $since) { continue }
            $res = @($d.Resources) -join ' '
            if ($res -match [regex]::Escape($Path) -or $res -match '(?i)deep-fried-chicken') { return $d }
        }
    }
    catch { }
    return $null
}

$script:DefenderExcluded = @{}

# Returns $true when the exclusion is in place (added now, or already added this run).
function Request-DefenderExclusion
{
    param([string[]] $Paths, [string] $Because)

    $todo = @()
    foreach ($p in $Paths) { if (-not $script:DefenderExcluded.ContainsKey($p.ToLowerInvariant())) { $todo += $p } }
    if ($todo.Count -eq 0) { return $true }

    Write-Host ''
    Write-Chunk '  Windows Defender removed a file this install needs.' 'Yellow'
    Write-Chunk ('  ' + $Because) 'Yellow'
    Write-Chunk '  This is a known false positive: a neural consumer interposes on NVIDIA''s NGX runtime,' 'DarkGray'
    Write-Chunk '  which antivirus heuristics dislike. The fix is a Defender exclusion' 'DarkGray'
    Write-Chunk '  (Settings > Virus & threat protection > Exclusions) for:' 'DarkGray'
    foreach ($p in $todo) { Write-Chunk ('      ' + $p) 'White' }
    Write-Chunk '  Adding it needs administrator rights (a UAC prompt). Nothing else is changed.' 'DarkGray'

    if ($NoElevate) {
        $cmd = ($todo | ForEach-Object { 'Add-MpPreference -ExclusionPath ' + (ConvertTo-PsLiteral $_) }) -join '; '
        Report -Status 'Fail' -Text 'Defender exclusion not added (-NoElevate).' `
               -Manual ('From an elevated PowerShell: ' + $cmd + "`nThen re-run this installer.")
        return $false
    }
    if (-not (Confirm-Step 'Add the Defender exclusion now?')) {
        $cmd = ($todo | ForEach-Object { 'Add-MpPreference -ExclusionPath ' + (ConvertTo-PsLiteral $_) }) -join '; '
        Report -Status 'Fail' -Text 'Defender exclusion declined.' `
               -Manual ('Add it yourself (elevated PowerShell): ' + $cmd + "`nThen re-run this installer.")
        return $false
    }

    $lines = ($todo | ForEach-Object { '  Add-MpPreference -ExclusionPath ' + (ConvertTo-PsLiteral $_) }) -join "`r`n"
    $r = Invoke-Elevated -Script $lines -What 'add a Windows Defender exclusion'
    if ($r -eq 'ok') {
        foreach ($p in $todo) { $script:DefenderExcluded[$p.ToLowerInvariant()] = $true }
        Report -Status 'Done' -Text ('Defender exclusion added for: ' + ($todo -join ', '))
        # Defender may also have quarantined the file already; restoring is not automated.
        return $true
    }
    if ($r -eq 'declined') { Report -Status 'Fail' -Text 'The UAC prompt was declined; no exclusion added.'; return $false }
    Report -Status 'Fail' -Text 'Adding the Defender exclusion failed.' -Detail 'Tamper Protection or a policy may forbid it. Add it by hand in Windows Security > Exclusions.'
    return $false
}

# ---------------------------------------------------------------------------------------
# 0. Resolve the target
# ---------------------------------------------------------------------------------------

Write-Banner

$exeSkip = '(?i)(launcher|unins|setup|crash|redist|vcredist|dxsetup|dxwebsetup|dgvoodoocpl|touchup|prereq|activation|helper|updater|report|dlss5-feed-host)'

# No argument: the script was most likely dropped into the game folder. Look next to it,
# propose what is there, and ask before using it.
if (-not $GameExe -or -not $GameExe.Trim()) {
    $here = $null
    try { $here = $PSScriptRoot } catch { }
    if (-not $here) { try { $here = Split-Path -Parent $MyInvocation.MyCommand.Path } catch { } }
    $found = @()
    if ($here -and (Test-DirHere $here)) {
        $found = @(Get-ChildItem -LiteralPath $here -File -Filter '*.exe' -ErrorAction SilentlyContinue |
                   Where-Object { $_.Name -notmatch $exeSkip } | Sort-Object Length -Descending)
    }
    if ($found.Count -gt 0) {
        Write-Chunk ('  Found next to this script (' + $here + '):') 'White'
        $i = 1
        foreach ($f in $found) {
            $peF = Get-PeInfo $f.FullName
            $arch = ''
            if ($peF -and $peF.Bits -gt 0) { $arch = ', ' + $peF.Bits + '-bit' }
            Write-Chunk ('   ' + $i + '. ' + $f.Name + ' (' + (Format-Size $f.Length) + $arch + ')') $(if ($i -eq 1) { 'Gray' } else { 'DarkGray' })
            $i++
        }
        if ($Yes) { $GameExe = $found[0].FullName }
        else {
            Write-Chunk ('  Use ' + $found[0].Name + ' as the game executable? [Y/n, or a number from the list] ') 'Cyan' -NoNewline
            try { $a = Read-Host } catch { $a = '' }
            $a = $a.Trim()
            if ($a -eq '' -or $a -match '^(?i)y(es)?$') { $GameExe = $found[0].FullName }
            elseif ($a -match '^\d+$' -and [int]$a -ge 1 -and [int]$a -le $found.Count) { $GameExe = $found[[int]$a - 1].FullName }
            else { $GameExe = '' }
        }
    }
    if (-not $GameExe) {
        Write-Chunk '  Path to the game''s .exe (the real one, not a launcher): ' 'Cyan' -NoNewline
        try { $GameExe = Read-Host } catch { $GameExe = '' }
        $GameExe = $GameExe.Trim().Trim('"')
    }
}
if (-not $GameExe) { Stop-Install -Text 'No game executable given.' -Manual 'Drop this script into the game folder and run it, or pass the path of the game''s .exe.' }

try { $resolved = (Resolve-Path -LiteralPath $GameExe -ErrorAction Stop).ProviderPath } catch { $resolved = $null }
if (-not $resolved) { Stop-Install -Text ('Path not found: ' + $GameExe) }

$gameDir = $null
$exePath = $null
$exeGuessed = $false

if (Test-DirHere $resolved) {
    $gameDir = $resolved
    $exes = @(Get-ChildItem -LiteralPath $gameDir -File -Filter '*.exe' -ErrorAction SilentlyContinue |
              Where-Object { $_.Name -notmatch $exeSkip } | Sort-Object Length -Descending)
    if ($exes.Count -eq 0) { Stop-Install -Text ('No .exe found in ' + $gameDir) -Manual 'Pass the path of the game''s executable.' }
    $exePath = $exes[0].FullName
    $exeGuessed = $true
}
elseif ($resolved -match '(?i)\.exe$') {
    $exePath = $resolved
    $gameDir = [IO.Path]::GetDirectoryName($resolved)
}
else {
    Stop-Install -Text ('Not an .exe and not a folder: ' + $resolved)
}

# Unreal Engine bootstrap: a tiny <Game>.exe next to <Game>\Binaries\Win64\<Game>-Win64-Shipping.exe.
try {
    $leafNoExt = [IO.Path]::GetFileNameWithoutExtension($exePath)
    foreach ($sub in @('Win64', 'WinGDK', 'Win32')) {
        $cand = Join-Safe $gameDir ($leafNoExt + '\Binaries\' + $sub + '\' + $leafNoExt + '-' + $sub + '-Shipping.exe')
        if (Test-FileHere $cand) {
            Report -Status 'Info' -Text ('Unreal Engine bootstrap detected; using the real executable instead.') -Detail $cand
            $exePath = $cand
            $gameDir = [IO.Path]::GetDirectoryName($cand)
            $exeGuessed = $false
            break
        }
    }
}
catch { }

if ($gameDir -and $gameDir.StartsWith($env:WINDIR, [StringComparison]::OrdinalIgnoreCase)) {
    Stop-Install -Text 'Refusing to install into the Windows directory.'
}

Write-Chunk '  Game    ' 'DarkGray' -NoNewline
Write-Host $exePath
if ($exeGuessed) { Write-Chunk '          (picked as the largest non-launcher .exe in that folder; pass the .exe itself to override)' 'DarkGray' }

# Cache folder
if (-not $Downloads) { $Downloads = Join-Safe $env:LOCALAPPDATA 'DLSS5-Feeder\downloads' }
$script:Cache = $Downloads
New-DirSafe $script:Cache
Write-Chunk '  Cache   ' 'DarkGray' -NoNewline
Write-Host $script:Cache

# Which neural consumer? Asked here, before anything is downloaded, because the answer
# decides what gets fetched and what must never be installed beside it.
if ($Consumer -eq 'Ask') {
    if ($Yes) {
        $Consumer = 'DFC'
    }
    else {
        Write-Host ''
        Write-Chunk '  Neural consumer -- the add-on that turns the feed into neural rendering.' 'White'
        Write-Chunk '   1. Deep Fried Chicken  ' 'Gray' -NoNewline
        Write-Chunk 'recommended; negotiates with the feeder over its own interop ABI' 'DarkGray'
        Write-Chunk '   2. RenoDX DLSS 5       ' 'Gray' -NoNewline
        Write-Chunk 'Krish''s renodx-dlss5 add-on' 'DarkGray'
        Write-Chunk '  Exactly one of them may be installed: each goes inert, or misbehaves, beside the other.' 'DarkGray'
        Write-Chunk '  Which one? [1/2, Enter for 1] ' 'Cyan' -NoNewline
        try { $a = Read-Host } catch { $a = '' }
        switch ($a.Trim()) {
            '2'      { $Consumer = 'RenoDX' }
            'renodx' { $Consumer = 'RenoDX' }
            default  { $Consumer = 'DFC' }
        }
    }
}
$consumerLabel = if ($Consumer -eq 'DFC') { 'Deep Fried Chicken' } else { 'RenoDX DLSS 5' }
Write-Chunk '  Neural  ' 'DarkGray' -NoNewline
Write-Host $consumerLabel
if ($LocalFiles) {
    if (-not (Test-DirHere $LocalFiles)) { Stop-Install -Text ('-LocalFiles folder not found: ' + $LocalFiles) }
    Write-Chunk '  Local   ' 'DarkGray' -NoNewline
    Write-Host $LocalFiles
}

# Running game?
try {
    $running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { try { $_.MainModule.FileName -ieq $exePath } catch { $false } })
    if ($running.Count -gt 0) { Stop-Install -Text 'The game is running. Close it first; its files are locked.' }
}
catch { }

# ---------------------------------------------------------------------------------------
# 1. Architecture and render API
# ---------------------------------------------------------------------------------------

Write-Section 'Game executable and render API'

$pe = Get-PeInfo $exePath
if ($null -eq $pe) { Stop-Install -Text 'The executable''s PE header is unreadable.' }
if ($pe.Bits -eq 0) { Stop-Install -Text ('Unsupported architecture: ' + $pe.Arch) }
$gameBits = $pe.Bits
Report -Status 'Ok' -Text ('Executable: ' + [IO.Path]::GetFileName($exePath) + ' (' + $pe.Arch + ', ' + $gameBits + '-bit)')

# ReShade's compatibility list: known per-game overrides and bans.
$compat = $null
$compatText = Get-WebString $Sources.CompatIni
$exeLeaf = [IO.Path]::GetFileName($exePath)
if ($compatText) {
    $banned  = Get-IniKey -Text $compatText -Section $exeLeaf -Key 'Banned'
    $target  = Get-IniKey -Text $compatText -Section $exeLeaf -Key 'InstallTarget'
    $rapi    = Get-IniKey -Text $compatText -Section $exeLeaf -Key 'RenderApi'
    $is64    = Get-IniKey -Text $compatText -Section $exeLeaf -Key 'Is64Bit'
    if ($banned -eq '1') {
        Stop-Install -Text 'ReShade''s compatibility list says this game bans or blocks ReShade.' -Detail 'Installing anyway risks an account ban. Not continuing.'
    }
    if ($target) {
        Stop-Install -Text ('ReShade''s compatibility list says this game needs ReShade installed into a sub-folder (' + $target + ').') `
                     -Detail 'This installer does not handle that layout yet. Run ReShade''s own installer for this game, then deploy the feeder by hand (see the README).'
    }
    if ($rapi) { $compat = $rapi; Report -Status 'Info' -Text ('ReShade''s compatibility list knows this exe: RenderApi=' + $rapi) }
    if ($is64 -eq '1' -and $gameBits -ne 64) { $gameBits = 64; Report -Status 'Info' -Text 'Compatibility list overrides the bitness to 64.' }
    if ($is64 -eq '0' -and $gameBits -ne 32) { $gameBits = 32; Report -Status 'Info' -Text 'Compatibility list overrides the bitness to 32.' }
}
else {
    Report -Status 'Info' -Text 'ReShade''s compatibility list could not be fetched (offline?); relying on the import table alone.'
}

# What the exe imports.
$imp = @($pe.Imports)
$impD3D8   = [bool]($imp | Where-Object { $_ -match '(?i)^d3d8' })
$impD3D9   = [bool]($imp | Where-Object { $_ -match '(?i)^d3d9' })
$impDXGI   = [bool]($imp | Where-Object { $_ -match '(?i)^(dxgi|d3d1[012])' -or $_ -match 'GFSDK' })
$impGL     = [bool]($imp | Where-Object { $_ -match '(?i)^opengl32' })
$impVK     = [bool]($imp | Where-Object { $_ -match '(?i)^vulkan-1' })
$impDDraw  = [bool]($imp | Where-Object { $_ -match '(?i)^ddraw' })

$gfx = @($imp | Where-Object { $_ -match '(?i)^(d3d|dxgi|opengl32|vulkan-1|ddraw)' })
if ($gfx.Count -gt 0) { Report -Status 'Info' -Text ('Graphics imports: ' + ($gfx -join ', ')) }
else { Report -Status 'Info' -Text 'No graphics API in the static import table (the engine loads it at run time).' }

# Local translation layers.
$dxvk = $false
foreach ($n in @('d3d9.dll', 'dxgi.dll', 'd3d11.dll', 'd3d10core.dll')) {
    $p = Find-FileIn $gameDir $n
    if ($p -and ((Get-ProductNameSafe $p) -match '(?i)dxvk')) { $dxvk = $true }
}
$dgVoodooPresent = [bool]((Find-FileIn $gameDir 'dgVoodoo.conf') -and (Find-FileIn $gameDir 'd3d9.dll'))

$detected = 'Unknown'
if ($compat) {
    switch -Regex ($compat) {
        '^D3D8$'                    { $detected = 'D3D8' }
        '^D3D9$'                    { $detected = 'D3D9' }
        '^(D3D1[012]|DXGI)$'        { $detected = 'D3D' }
        '^OpenGL$'                  { $detected = 'OpenGL' }
        '^Vulkan$'                  { $detected = 'Vulkan' }
    }
}
if ($detected -eq 'Unknown') {
    if ($impVK) { $detected = 'Vulkan' }
    elseif ($impDXGI) { $detected = 'D3D' }
    elseif ($impD3D9) { $detected = 'D3D9' }
    elseif ($impD3D8) { $detected = 'D3D8' }
    elseif ($impGL) { $detected = 'OpenGL' }
    elseif ($impDDraw) { $detected = 'DDraw' }
}

# Second level: engines that LoadLibrary their API at run time (Max Payne 3, many RAGE /
# in-house engines) import nothing. Look for the DLL names as strings in the exe instead.
#
# And where the exe is a small launcher shim that only starts the real engine -- Watch Dogs
# ships a 136 KB Watch_Dogs.exe next to the Disrupt binaries -- the exe contains no such
# string either, and scanning it alone found nothing at all. So fall through to the DLLs
# beside it, largest first, since the renderer lives in the big one.
$ambiguous = $false
if ($detected -eq 'Unknown') {
    $hintSource = 'the executable'
    $hints = Get-ApiStringHints $exePath
    if ($hints.Count -eq 0) {
        # Two things make this harder than "read the biggest DLL". A graphics RUNTIME or
        # proxy sitting in the folder answers with its own name -- ReShade's dxgi.dll, DXVK,
        # dgVoodoo2, or the NGX runtimes this very installer puts there on an earlier run
        # (nvngx_dlssnr.dll is 165 MB, so it sorts first and says Vulkan). And a game can
        # ship a GPU-detection helper that names every API it has ever heard of: Watch Dogs'
        # systemdetection64.dll claims OpenGL and Direct3D 9 while the engine is D3D11.
        #
        # So: skip anything that IS a graphics runtime, read the rest, and add the counts up
        # rather than trusting whichever file happened to be largest. The precedence below
        # then resolves a folder that mentions several.
        $skipExact = @('dxgi.dll','d3d8.dll','d3d9.dll','d3d10.dll','d3d10_1.dll','d3d11.dll','d3d12.dll',
                       'opengl32.dll','vulkan-1.dll','ddraw.dll','dinput8.dll','winmm.dll','version.dll')
        $skipLike  = @('nvngx_*','reshade*','deep-fried-chicken*','*dlss5*','alexs-toolkit*','dxvk*',
                       'd3dcompiler*','nvapi*','amd_ags*','_nvngx*')
        $scanned = @()
        try {
            $scanned = @(Get-ChildItem -LiteralPath $gameDir -File -Filter '*.dll' -ErrorAction SilentlyContinue |
                         Where-Object { $_.Length -gt 262144 } |
                         Where-Object { $skipExact -notcontains $_.Name.ToLowerInvariant() } |
                         Where-Object { $n = $_.Name; -not (@($skipLike | Where-Object { $n -like $_ }).Count) } |
                         Sort-Object Length -Descending | Select-Object -First 8)
        }
        catch { }
        $from = @()
        foreach ($nb in $scanned) {
            $h2 = Get-ApiStringHints $nb.FullName
            if ($h2.Count -eq 0) { continue }
            $from += $nb.Name
            foreach ($k in $h2.Keys) {
                if ($hints.ContainsKey($k)) { $hints[$k] = $hints[$k] + $h2[$k] } else { $hints[$k] = $h2[$k] }
            }
        }
        if ($hints.Count -gt 0) {
            $hintSource = ($from -join ' + ')
            Report -Status 'Info' -Text ('The executable names no graphics API; read it out of ' + $hintSource + ' instead.') `
                   -Detail 'A small launcher exe that starts the real engine carries no API strings of its own.'
        }
    }
    if ($hints.Count -gt 0) {
        Report -Status 'Info' -Text ('API DLL names found inside ' + $hintSource + ': ' + (($hints.Keys | Sort-Object | ForEach-Object { $_ + ' x' + $hints[$_] }) -join ', '))
        $hVK  = $hints.ContainsKey('vulkan-1.dll')
        $hDX  = $hints.ContainsKey('dxgi.dll') -or $hints.ContainsKey('d3d11.dll') -or $hints.ContainsKey('d3d12.dll') -or
                $hints.ContainsKey('d3d10.dll') -or $hints.ContainsKey('d3d10_1.dll') -or $hints.ContainsKey('d3d10core.dll')
        $hD9  = $hints.ContainsKey('d3d9.dll')
        $hD8  = $hints.ContainsKey('d3d8.dll')
        $hGL  = $hints.ContainsKey('opengl32.dll')
        if ($hVK) { $detected = 'Vulkan' }
        elseif ($hDX) { $detected = 'D3D'; if ($hD9) { $ambiguous = $true } }
        elseif ($hD9) { $detected = 'D3D9' }
        elseif ($hD8) { $detected = 'D3D8' }
        elseif ($hGL) { $detected = 'OpenGL' }
        if ($detected -ne 'Unknown') {
            $d = 'This is a guess from strings, not from the import table.'
            if ($ambiguous) { $d += "`nBoth Direct3D 9 and 10/11/12 are named there: the game can run either. Direct3D 10/11/12 is assumed (ReShade makes the same choice); if the game is set to DirectX 9, re-run with -Api D3D9." }
            Report -Status 'Warn' -Text ('Render API guessed as ' + $detected + ' from strings in ' + $hintSource + '.') -Detail $d
        }
    }
}

if ($dxvk) { $detected = 'Vulkan'; Report -Status 'Info' -Text 'DXVK is installed next to the exe, so the game reaches the GPU through Vulkan.' }
if ($dgVoodooPresent -and $detected -match '^D3D[89]$') { Report -Status 'Info' -Text 'dgVoodoo2 is already installed next to the exe.' }

if ($Api -ne 'Auto') {
    if ($detected -ne 'Unknown' -and $detected -ne $Api) {
        Report -Status 'Warn' -Text ('-Api ' + $Api + ' overrides the detected API (' + $detected + ').')
    }
    $useApi = $Api
}
else {
    $useApi = $detected
}

# Still nothing. This script already asks which executable and which neural consumer, so
# stopping dead here -- and making the user re-run the whole thing with -Api -- was the one
# unanswerable question in an otherwise interactive install. Ask it instead.
if ($useApi -eq 'Unknown' -and -not $Yes) {
    Write-Host ''
    Write-Chunk '  Which render API does this game use?' 'White'
    Write-Chunk '  Nothing in the executable or the files beside it says, which usually means the engine' 'DarkGray'
    Write-Chunk '  loads it at run time. If you are not sure: almost every 64-bit game of the last decade' 'DarkGray'
    Write-Chunk '  is Direct3D 10/11/12, and the game''s own graphics options usually name it.' 'DarkGray'
    Write-Chunk '   1. Direct3D 10/11/12' 'Gray'
    Write-Chunk '   2. Vulkan' 'Gray'
    Write-Chunk '   3. OpenGL' 'Gray'
    Write-Chunk '   4. Direct3D 9   ' 'Gray' -NoNewline
    Write-Chunk '(installed through dgVoodoo2)' 'DarkGray'
    Write-Chunk '   5. Direct3D 8   ' 'Gray' -NoNewline
    Write-Chunk '(installed through dgVoodoo2)' 'DarkGray'
    Write-Chunk '  Which one? [1-5, Enter for 1] ' 'Cyan' -NoNewline
    try { $a = Read-Host } catch { $a = '' }
    $useApi = switch ($a.Trim()) {
        '2'      { 'Vulkan' }
        '3'      { 'OpenGL' }
        '4'      { 'D3D9' }
        '5'      { 'D3D8' }
        default  { 'D3D' }
    }
    Report -Status 'Info' -Text ('Render API set to ' + $useApi + ' by hand.') `
           -Detail 'If the install completes but the game shows no ReShade overlay, this was the wrong answer -- re-run with -Api and the right one.'
}

switch ($useApi) {
    'Unknown' { Stop-Install -Text 'Could not tell which render API the game uses.' -Manual 'Re-run with -Api D3D, -Api Vulkan, -Api OpenGL, -Api D3D9 or -Api D3D8.' }
    'DDraw'   { Stop-Install -Text 'DirectDraw games are not supported by DLSS5-Feeder.' }
}

$apiLabel = switch ($useApi) {
    'D3D'    { 'Direct3D 10/11/12 (local ReShade dxgi.dll)' }
    'Vulkan' { 'Vulkan (machine-wide ReShade layer)' }
    'OpenGL' { 'OpenGL (local ReShade opengl32.dll)' }
    'D3D9'   { 'Direct3D 9 via dgVoodoo2 (local ReShade dxgi.dll)' }
    'D3D8'   { 'Direct3D 8 via dgVoodoo2 (local ReShade dxgi.dll)' }
}
Report -Status 'Ok' -Text ('Render API: ' + $apiLabel)

$isVulkan  = ($useApi -eq 'Vulkan')
$isGL      = ($useApi -eq 'OpenGL')
$isDgV     = ($useApi -eq 'D3D9' -or $useApi -eq 'D3D8')
$is32      = ($gameBits -eq 32)
if ($is32 -and $isVulkan) { Report -Status 'Info' -Text '32-bit Vulkan: ReShade goes in as the 32-bit layer, and the feeder as addon32 + host64\.' }

# Where the 64-bit side (consumer + NGX) lives.
$hostDir = Join-Safe $gameDir 'host64'
if ($is32) { $consumerDir = $hostDir; $consumerWhere = 'host64\' } else { $consumerDir = $gameDir; $consumerWhere = 'the game folder' }
$shaderDir  = Join-Safe $gameDir 'reshade-shaders\Shaders'
$textureDir = Join-Safe $gameDir 'reshade-shaders\Textures'

# ---------------------------------------------------------------------------------------
# 2. GPU
# ---------------------------------------------------------------------------------------

Write-Section 'GPU'
$gpus = $null
try { $gpus = Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop } catch { try { $gpus = Get-WmiObject -Class Win32_VideoController -ErrorAction Stop } catch { } }
if ($gpus) {
    $rtx = @($gpus | Where-Object { $_.Name -and $_.Name -match '(?i)nvidia' -and $_.Name -match '(?i)(RTX|TITAN RTX)' })
    if ($rtx.Count -gt 0) { Report -Status 'Ok' -Text ('NVIDIA RTX adapter: ' + (($rtx | ForEach-Object { $_.Name }) -join '; ')) }
    else { Report -Status 'Warn' -Text 'No NVIDIA RTX adapter detected. DLSS needs one; installing anyway.' -Detail (@($gpus | ForEach-Object { $_.Name }) -join '; ') }
}
else { Report -Status 'Warn' -Text 'Could not query the display adapters.' }

# ---------------------------------------------------------------------------------------
# 3. Downloads
# ---------------------------------------------------------------------------------------

Write-Section 'Downloads'

# 3a. Feeder release
$feederPath = $null
if ($FeederZip) {
    $feederPath = Resolve-Piece -Label 'DLSS5-Feeder release' -Explicit $FeederZip -CacheName 'DLSS5-Feeder-explicit.zip'
}
else {
    $local = $null
    if ($LocalFiles) {
        $hit = Get-ChildItem -LiteralPath $LocalFiles -File -Filter 'DLSS5-Feeder-*.zip' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($hit) { $local = $hit.FullName; Report -Status 'Ok' -Text ('DLSS5-Feeder release: using ' + $local) }
    }
    if ($local) { $feederPath = $local }
    else {
        $relJson = Get-WebString ($Sources.FeederReleases + $(if ($Prerelease) { '?per_page=5' } else { '/latest' }))
        $assetUrl = $null
        $assetName = $null
        if ($relJson) {
            try {
                $rel = $relJson | ConvertFrom-Json
                if ($Prerelease) { $rel = @($rel)[0] }
                foreach ($a in @($rel.assets)) {
                    if ($a.name -match '(?i)^DLSS5-Feeder-.*\.zip$') { $assetUrl = $a.browser_download_url; $assetName = $a.name; break }
                }
                if ($assetUrl) { Report -Status 'Info' -Text ('Latest release: ' + $rel.tag_name + ' (' + $assetName + ')') }
            }
            catch { }
        }
        if ($assetUrl) {
            $dest = Join-Safe $script:Cache $assetName
            if (Get-Download -Url $assetUrl -Dest $dest -Label 'DLSS5-Feeder release') { $feederPath = $dest }
        }
        else {
            # Offline: fall back to the newest cached release.
            $hit = Get-ChildItem -LiteralPath $script:Cache -File -Filter 'DLSS5-Feeder-*.zip' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
            if ($hit) { $feederPath = $hit.FullName; Report -Status 'Warn' -Text ('GitHub unreachable; using the cached release ' + $hit.Name) }
            else { Report -Status 'Fail' -Text 'Could not find the DLSS5-Feeder release on GitHub.' -Detail 'Download DLSS5-Feeder-<version>.zip from https://github.com/jlrouzies-fr/DLSS5-Feeder/releases and pass it with -FeederZip.' }
        }
    }
}
if (-not $feederPath) { Stop-Install -Text 'No feeder release to install.' }

# 3b. ReShade setup (only needed if ReShade is missing or too old somewhere; cheap, always fetch)
$reshadePath = $null
if ($ReShadeSetup) {
    $reshadePath = Resolve-Piece -Label 'ReShade setup' -Explicit $ReShadeSetup -CacheName 'ReShade_Setup_explicit_Addon.exe'
}
else {
    $local = $null
    if ($LocalFiles) {
        $hit = Get-ChildItem -LiteralPath $LocalFiles -File -Filter 'ReShade_Setup_*_Addon.exe' -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
        if ($hit) { $local = $hit.FullName; Report -Status 'Ok' -Text ('ReShade setup: using ' + $local) }
    }
    if ($local) { $reshadePath = $local }
    else {
        $homePage = Get-WebString $Sources.ReShadeHome
        $setupRel = $null
        if ($homePage) {
            $m = [regex]::Match($homePage, 'downloads/ReShade_Setup_([0-9.]+)_Addon\.exe')
            if ($m.Success) { $setupRel = $m.Value; Report -Status 'Info' -Text ('reshade.me offers ReShade ' + $m.Groups[1].Value + ' (add-on build)') }
        }
        if ($setupRel) { $url = $Sources.ReShadeHome + '/' + $setupRel } else { $url = $Sources.ReShadeFallback }
        $dest = Join-Safe $script:Cache ([IO.Path]::GetFileName(($url -split '\?')[0]))
        if (Get-Download -Url $url -Dest $dest -Label 'ReShade setup') { $reshadePath = $dest }
    }
}

# 3c. Framework headers
$headerPaths = @{}
foreach ($h in @('ReShade.fxh', 'ReShadeUI.fxh', 'DrawText.fxh')) {
    if (Find-FileUnder $shaderDir $h) { $headerPaths[$h] = $null; continue }   # already installed
    $p = Resolve-Piece -Label $h -LocalPattern $h -DefaultUrl ($Sources.Headers + $h) -CacheName ('reshade-framework\' + $h)
    $headerPaths[$h] = $p
}

# 3d. LumeniteFX
$lumenitePath = Resolve-Piece -Label 'LumeniteFX' -Explicit $LumeniteZip -LocalPattern 'LumeniteFX*.zip' -DefaultUrl $Sources.Lumenite -CacheName 'LumeniteFX-mainline.zip'

# 3e. Consumer
$dfcPath = $null
$renoPath = $null
if ($Consumer -eq 'DFC') {
    $dfcPath = Resolve-Piece -Label 'Deep Fried Chicken' -Explicit $DfcZip -LocalPattern 'Deep-Fried-Chicken*.zip' -DefaultUrl $Sources.Dfc -CacheName 'Deep-Fried-Chicken.zip'
    if (-not $dfcPath -and -not $DfcZip) {
        # Was the download eaten by Defender? Then the .zip never landed; the download reports
        # the block, and Get-MpThreatDetection confirms it.
        $det = Get-DefenderDetection (Join-Safe $script:Cache 'Deep-Fried-Chicken.zip')
        if ($det) {
            $ok = Request-DefenderExclusion -Paths @($script:Cache, $gameDir) -Because 'It blocked the Deep Fried Chicken download into the cache folder.'
            if ($ok) { $dfcPath = Resolve-Piece -Label 'Deep Fried Chicken (retry)' -Explicit $DfcZip -LocalPattern 'Deep-Fried-Chicken*.zip' -DefaultUrl $Sources.Dfc -CacheName 'Deep-Fried-Chicken.zip' }
        }
    }
    if (-not $dfcPath) {
        Report -Status 'Fail' -Text 'Deep Fried Chicken is not available.' `
               -Detail ('Get the zip from its Discord (' + $Sources.DfcDiscord + ') and pass it with -DfcZip, or drop it in -LocalFiles.')
    }
}
else {
    $renoPath = Resolve-Piece -Label 'RenoDX DLSS 5 add-on' -Explicit $RenoDxAddon -LocalPattern 'renodx-dlss5.addon64' `
                              -DefaultUrl $Sources.RenoDxDlss5 -CacheName 'renodx-dlss5.addon64'
    if (-not $renoPath) {
        Report -Status 'Fail' -Text 'renodx-dlss5.addon64 is not available.' `
               -Detail ('Get it from the RenoDX Discord (' + $Sources.RenoDxDiscord + ') or the RHI installer, and pass it with -RenoDxAddon or via -LocalFiles.')
    }
}

# 3f. NVIDIA runtimes
$dlssNrPath = Resolve-Piece -Label 'nvngx_dlssnr.dll' -Explicit $DlssNrDll -LocalPattern 'nvngx_dlssnr.dll' -DefaultUrl $Sources.DlssNr -CacheName 'nvngx_dlssnr.dll'
$dlssPath   = Resolve-Piece -Label 'nvngx_dlss.dll'   -Explicit $DlssDll   -LocalPattern 'nvngx_dlss.dll'   -DefaultUrl $Sources.Dlss   -CacheName 'nvngx_dlss.dll'
if (-not $dlssNrPath) { Report -Status 'Fail' -Text 'nvngx_dlssnr.dll is not available.' -Detail ('It is on the RenoDX Discord (' + $Sources.RenoDxDiscord + '). Pass it with -DlssNrDll.') }
if (-not $dlssPath)   { Report -Status 'Fail' -Text 'nvngx_dlss.dll is not available.'   -Detail 'Any DLSS-enabled game ships one, or use DLSS Swapper. Pass it with -DlssDll.' }

# 3g. dgVoodoo2
$dgvPath = $null
if ($isDgV -and -not $dgVoodooPresent) {
    if ($DgVoodooZip) {
        $dgvPath = Resolve-Piece -Label 'dgVoodoo2' -Explicit $DgVoodooZip -CacheName 'dgVoodoo2-explicit.zip'
    }
    else {
        $local = $null
        if ($LocalFiles) {
            $hit = Get-ChildItem -LiteralPath $LocalFiles -File -Filter 'dgVoodoo2_*.zip' -ErrorAction SilentlyContinue | Where-Object { $_.Name -notmatch '(?i)(dbg|dev64)' } | Sort-Object Name -Descending | Select-Object -First 1
            if ($hit) { $local = $hit.FullName; Report -Status 'Ok' -Text ('dgVoodoo2: using ' + $local) }
        }
        if ($local) { $dgvPath = $local }
        else {
            $j = Get-WebString $Sources.DgVoodoo
            $url = $null; $name = $null
            if ($j) {
                try {
                    $rel = $j | ConvertFrom-Json
                    foreach ($a in @($rel.assets)) { if ($a.name -match '(?i)^dgVoodoo2_\d+_\d+\.zip$') { $url = $a.browser_download_url; $name = $a.name; break } }
                    if ($url) { Report -Status 'Info' -Text ('dgVoodoo2 latest: ' + $rel.tag_name + ' (' + $name + ')') }
                }
                catch { }
            }
            if ($url) { $dest = Join-Safe $script:Cache $name; if (Get-Download -Url $url -Dest $dest -Label 'dgVoodoo2') { $dgvPath = $dest } }
            else {
                $hit = Get-ChildItem -LiteralPath $script:Cache -File -Filter 'dgVoodoo2_*.zip' -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
                if ($hit) { $dgvPath = $hit.FullName; Report -Status 'Warn' -Text ('GitHub unreachable; using cached ' + $hit.Name) }
                else { Report -Status 'Fail' -Text 'dgVoodoo2 could not be fetched.' -Detail 'Download it from https://github.com/dege-diosg/dgVoodoo2/releases and pass -DgVoodooZip.' }
            }
        }
    }
}

# ---------------------------------------------------------------------------------------
# 4. ReShade
# ---------------------------------------------------------------------------------------

Write-Section 'ReShade'

function Test-ReShadeVersionOk
{
    param([string] $Version)
    if (-not $Version) { return $null }
    $m = [regex]::Match($Version, '^\s*(\d+)\.(\d+)')
    if (-not $m.Success) { return $null }
    $want = $ReShadeMinVersion -split '\.'
    $maj = [int]$m.Groups[1].Value; $min = [int]$m.Groups[2].Value
    if ($maj -gt [int]$want[0]) { return $true }
    if ($maj -eq [int]$want[0] -and $min -ge [int]$want[1]) { return $true }
    return $false
}

function Test-IsReShade
{
    param([string] $Path)
    $n = Get-ProductNameSafe $Path
    return [bool]($n -and $n -match '(?i)reshade')
}

$reshadeZip = $null
$reshadeSetupVersion = $null
if ($reshadePath) {
    try {
        $reshadeZip = Open-Zip $reshadePath
        $m = [regex]::Match([IO.Path]::GetFileName($reshadePath), 'ReShade_Setup_([0-9.]+)')
        if ($m.Success) { $reshadeSetupVersion = $m.Groups[1].Value }
        if (-not (Find-ZipEntry $reshadeZip '^ReShade64\.dll$') -or -not (Find-ZipEntry $reshadeZip '^ReShade32\.dll$')) { throw 'ReShade32.dll / ReShade64.dll not found inside the setup' }
    }
    catch {
        Report -Status 'Fail' -Text 'The ReShade setup could not be opened as an archive.' -Detail $_.Exception.Message
        $reshadeZip = $null
    }
}

# Places a ReShade DLL of the given bitness at $To, unless a new-enough one is already there.
function Install-ReShadeDll
{
    param([int] $Bits, [string] $To, [string] $Label)

    $name = [IO.Path]::GetFileName($To)
    if (Test-FileHere $To) {
        if (Test-IsReShade $To) {
            $v = Get-FileVersionSafe $To
            $ok = Test-ReShadeVersionOk $v
            if ($ok -eq $true -and -not $Force) { Report -Status 'Ok' -Text ($Label + ': ReShade ' + $v + ' already present, kept.') -Detail $To; return $true }
            if ($ok -ne $true) { Report -Status 'Info' -Text ($Label + ': ReShade ' + $v + ' is too old (need ' + $ReShadeMinVersion + '+); replacing.') }
        }
        else {
            $what = Get-ProductNameSafe $To
            if (-not $what) { $what = 'no version info' }
            Report -Status 'Fail' -Text ($Label + ': a ' + $name + ' that is not ReShade is already there (' + $what + ').') `
                   -Detail $To -Manual ('Find out what that ' + $name + ' is. If the game needs it, ReShade must be installed under another name (see the README); otherwise move it away and re-run.')
            return $false
        }
    }
    if (-not $reshadeZip) { Report -Status 'Fail' -Text ($Label + ': no ReShade setup available to take ' + $name + ' from.'); return $false }
    $entry = Find-ZipEntry $reshadeZip ('^ReShade' + $Bits + '\.dll$')
    try {
        Expand-ZipEntry -Entry $entry -To $To
        Report -Status 'Done' -Text ($Label + ': ReShade ' + $reshadeSetupVersion + ' installed as ' + $name + '.') -Detail $To
        return $true
    }
    catch { Report -Status 'Fail' -Text ($Label + ': cannot write ' + $To) -Detail $_.Exception.Message; return $false }
}

$reshadeLocalDll = $null

if ($isVulkan) {
    # Machine-wide layer under C:\ProgramData\ReShade, registered in HKLM, gated per exe.
    $pdRoot = Join-Safe $env:ProgramData 'ReShade'
    $layerName = 'ReShade' + $gameBits
    $pdDll = Join-Safe $pdRoot ($layerName + '.dll')
    $pdJson = Join-Safe $pdRoot ($layerName + '.json')
    $regKey = if ($gameBits -eq 32) { 'HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\ImplicitLayers' } else { 'HKLM:\SOFTWARE\Khronos\Vulkan\ImplicitLayers' }

    $needDll = $true
    if (Test-FileHere $pdDll) {
        $v = Get-FileVersionSafe $pdDll
        if ((Test-ReShadeVersionOk $v) -eq $true -and -not $Force) { $needDll = $false; Report -Status 'Ok' -Text ('Vulkan layer ' + $layerName + '.dll ' + $v + ' already installed.') -Detail $pdDll }
        else { Report -Status 'Info' -Text ('Vulkan layer ' + $layerName + '.dll is ' + $v + '; will replace with ' + $reshadeSetupVersion + '.') }
    }
    $needJson = -not (Test-FileHere $pdJson)

    $needReg = $true
    try {
        $rp = Get-ItemProperty -Path $regKey -ErrorAction Stop
        foreach ($prop in $rp.PSObject.Properties) { if ($prop.Name -ieq $pdJson) { $needReg = $false } }
    }
    catch { }
    if (-not $needReg) { Report -Status 'Ok' -Text ('Vulkan layer registered in ' + $regKey) }

    $appsIni = Join-Safe $pdRoot 'ReShadeApps.ini'
    $needApps = $true
    $staleApps = @()
    $appsText = Read-TextSafe $appsIni
    if ($appsText) {
        $m = [regex]::Match($appsText, '(?im)^\s*Apps\s*=\s*(.*)$')
        if ($m.Success) {
            $entries = @($m.Groups[1].Value -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
            foreach ($e in $entries) {
                if ($e.TrimEnd('\') -ieq $exePath.TrimEnd('\')) { $needApps = $false }
                elseif (([IO.Path]::GetFileName($e) -ieq $exeLeaf) -and -not (Test-FileHere $e)) { $staleApps += $e }
            }
        }
    }
    if (-not $needApps) { Report -Status 'Ok' -Text 'Game exe already on the ReShadeApps.ini Apps= list.' }

    if ($needDll -or $needJson -or $needReg -or $needApps) {
        if (($needDll -or $needJson) -and -not $reshadeZip) {
            Report -Status 'Fail' -Text 'ReShade''s Vulkan layer is missing and no ReShade setup is available.'
        }
        else {
            # Stage the files non-elevated; the elevated step only copies and registers.
            $stage = Join-Safe $script:Cache ('reshade-' + $reshadeSetupVersion)
            $lines = @()
            $lines += '  $pd = ' + (ConvertTo-PsLiteral $pdRoot)
            $lines += '  New-Item -ItemType Directory -Force -Path $pd | Out-Null'
            if ($needDll -or $needJson) {
                foreach ($b in @(32, 64)) {
                    foreach ($ext in @('dll', 'json')) {
                        $n = 'ReShade' + $b + '.' + $ext
                        $e = Find-ZipEntry $reshadeZip ('^' + [regex]::Escape($n) + '$')
                        if (-not $e) { continue }
                        $sp = Join-Safe $stage $n
                        Expand-ZipEntry -Entry $e -To $sp
                        $target = Join-Safe $pdRoot $n
                        # Replace the DLL only for the bitness that needs it, or both when installing fresh; always place missing manifests.
                        $replace = ($ext -eq 'json' -and -not (Test-FileHere $target)) -or ($ext -eq 'dll' -and ($needDll -and ($b -eq $gameBits -or -not (Test-FileHere $target))))
                        if ($replace) { $lines += '  Copy-Item -LiteralPath ' + (ConvertTo-PsLiteral $sp) + ' -Destination ' + (ConvertTo-PsLiteral $target) + ' -Force' }
                    }
                }
                $lines += '  try { $acl = Get-Acl $pd; $sid = New-Object Security.Principal.SecurityIdentifier("S-1-15-2-1"); $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid, "ReadAndExecute", "ContainerInherit,ObjectInherit", "None", "Allow"))); Set-Acl $pd $acl } catch { }'
            }
            foreach ($b in @(32, 64)) {
                $k = if ($b -eq 32) { 'HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\ImplicitLayers' } else { 'HKLM:\SOFTWARE\Khronos\Vulkan\ImplicitLayers' }
                $j = Join-Safe $pdRoot ('ReShade' + $b + '.json')
                $lines += '  if (Test-Path -LiteralPath ' + (ConvertTo-PsLiteral $j) + ') { New-Item -Path ' + (ConvertTo-PsLiteral $k) + ' -Force | Out-Null; New-ItemProperty -Path ' + (ConvertTo-PsLiteral $k) + ' -Name ' + (ConvertTo-PsLiteral $j) + ' -Value 0 -PropertyType DWord -Force | Out-Null }'
            }
            if ($needApps) {
                $lines += '  $ini = Join-Path $pd "ReShadeApps.ini"'
                $lines += '  $exe = ' + (ConvertTo-PsLiteral $exePath)
                $lines += '  $stale = @(' + (($staleApps | ForEach-Object { ConvertTo-PsLiteral $_ }) -join ', ') + ')'
                $lines += '  if (Test-Path -LiteralPath $ini) { Copy-Item -LiteralPath $ini -Destination ($ini + ".bak") -Force; $t = [IO.File]::ReadAllText($ini); if ($t.Length -gt 0 -and [int]$t[0] -eq 0xFEFF) { $t = $t.Substring(1) } } else { $t = "" }'
                $lines += '  $m = [regex]::Match($t, "(?im)^\s*Apps\s*=\s*(.*)$")'
                $lines += '  if ($m.Success) { $list = @($m.Groups[1].Value -split "," | ForEach-Object { $_.Trim() } | Where-Object { $_ -and ($stale -notcontains $_) }) } else { $list = @() }'
                $lines += '  if (-not ($list | Where-Object { $_ -ieq $exe })) { $list += $exe }'
                $lines += '  $line = "Apps=" + ($list -join ",")'
                $lines += '  if ($m.Success) { $t = $t.Substring(0, $m.Index) + $line + $t.Substring($m.Index + $m.Length) } else { $t = ($t.TrimEnd() + "`r`n" + $line).TrimStart() }'
                $lines += '  [IO.File]::WriteAllText($ini, $t, (New-Object Text.UTF8Encoding $true))'
            }

            $what = 'install ReShade''s Vulkan layer'
            if (-not $needDll -and -not $needJson -and -not $needReg) { $what = 'add the game to ReShadeApps.ini' }
            if ($NoElevate) {
                Report -Status 'Fail' -Text ('Vulkan: cannot ' + $what + ' without administrator rights (-NoElevate).') `
                       -Manual ('Run ReShade''s installer (' + $reshadePath + ') against the game exe, choose Vulkan, enable add-ons -- or re-run this script without -NoElevate.')
            }
            else {
                $r = Invoke-Elevated -Script ($lines -join "`r`n") -What $what
                if ($r -eq 'ok') {
                    Report -Status 'Done' -Text ('Vulkan: ReShade layer ' + $(if ($needDll) { 'installed' } else { 'confirmed' }) + ', registered, and the game added to ReShadeApps.ini.') -Detail $pdRoot
                    if ($staleApps.Count -gt 0) { Report -Status 'Info' -Text ('Replaced stale Apps= entries for the same exe: ' + ($staleApps -join '; ')) }
                }
                elseif ($r -eq 'declined') {
                    Report -Status 'Fail' -Text 'Vulkan: the UAC prompt was declined; ReShade''s layer is not set up for this game.' `
                           -Manual ('Re-run and accept the prompt, or run ReShade''s installer (' + $reshadePath + ') against the exe with Vulkan selected.')
                }
                else {
                    Report -Status 'Fail' -Text 'Vulkan: the elevated step failed.' -Manual ('Run ReShade''s installer (' + $reshadePath + ') against the exe with Vulkan selected.')
                }
            }
        }
    }

    # A stray local ReShade DLL would fight the layer.
    foreach ($n in @('dxgi.dll', 'opengl32.dll', 'd3d11.dll')) {
        $p = Find-FileIn $gameDir $n
        if ($p -and (Test-IsReShade $p) -and -not $dxvk) {
            Report -Status 'Warn' -Text ('A local ReShade ' + $n + ' is also present; the Vulkan layer is what will load. Consider removing it.') -Detail $p
        }
    }
}
else {
    # Local DLL next to the exe: dxgi.dll for Direct3D (dgVoodoo owns d3d9.dll), opengl32.dll for OpenGL.
    $localName = if ($isGL) { 'opengl32.dll' } else { 'dxgi.dll' }

    # An existing ReShade under another name? Use it rather than double-installing.
    $existing = $null
    foreach ($n in @('dxgi.dll', 'opengl32.dll', 'd3d11.dll', 'd3d9.dll', 'd3d12.dll', 'ddraw.dll', 'dinput8.dll')) {
        $p = Find-FileIn $gameDir $n
        if ($p -and (Test-IsReShade $p)) { $existing = $p; break }
    }
    if ($existing -and ([IO.Path]::GetFileName($existing) -ine $localName)) {
        if ($isDgV -and ([IO.Path]::GetFileName($existing) -ieq 'd3d9.dll')) {
            Report -Status 'Warn' -Text 'ReShade is installed as d3d9.dll, but dgVoodoo2 needs that name.' -Detail $existing `
                   -Manual ('Remove ' + $existing + ' (it is ReShade, not the game''s) and re-run; ReShade then goes in as dxgi.dll.')
        }
        else {
            Report -Status 'Info' -Text ('ReShade is already installed here as ' + [IO.Path]::GetFileName($existing) + '; using that instead of ' + $localName + '.')
            $localName = [IO.Path]::GetFileName($existing)
        }
    }
    $reshadeLocalDll = Join-Safe $gameDir $localName
    $null = Install-ReShadeDll -Bits $gameBits -To $reshadeLocalDll -Label ('Game folder (' + $gameBits + '-bit)')
}

# 32-bit: the host helper needs its own 64-bit ReShade as dxgi.dll.
if ($is32) {
    New-DirSafe $hostDir
    $null = Install-ReShadeDll -Bits 64 -To (Join-Safe $hostDir 'dxgi.dll') -Label 'host64\'
}

# ---------------------------------------------------------------------------------------
# 5. dgVoodoo2 (Direct3D 8/9 only)
# ---------------------------------------------------------------------------------------

if ($isDgV) {
    Write-Section 'dgVoodoo2'
    $confPath = Join-Safe $gameDir 'dgVoodoo.conf'
    $wrapName = if ($useApi -eq 'D3D8') { 'D3D8.dll' } else { 'D3D9.dll' }
    $wrapPath = Join-Safe $gameDir $wrapName
    $sub = if ($gameBits -eq 64) { 'x64' } else { 'x86' }

    if ($dgVoodooPresent -and -not $Force) {
        Report -Status 'Ok' -Text 'dgVoodoo2 already installed; files kept, settings checked below.' -Detail $confPath
    }
    elseif ($dgvPath) {
        try {
            $z = Open-Zip $dgvPath
            try {
                $wanted = @(
                    @{ P = ('^MS/' + $sub + '/' + $wrapName + '$'); T = $wrapPath },
                    @{ P = '^dgVoodoo\.conf$'; T = $confPath },
                    @{ P = '^dgVoodooCpl\.exe$'; T = (Join-Safe $gameDir 'dgVoodooCpl.exe') }
                )
                foreach ($w in $wanted) {
                    $e = Find-ZipEntry $z $w.P
                    if (-not $e) { throw ('entry not found in the dgVoodoo2 zip: ' + $w.P) }
                    if (($w.T -ieq $wrapPath) -and (Test-FileHere $wrapPath) -and -not ((Get-ProductNameSafe $wrapPath) -match '(?i)dgvoodoo')) {
                        throw ('a ' + $wrapName + ' that is not dgVoodoo is already next to the exe (' + (Get-ProductNameSafe $wrapPath) + '); move it away first')
                    }
                    if (($w.T -ieq $confPath) -and (Test-FileHere $confPath)) { $null = Backup-File $confPath }
                    Expand-ZipEntry -Entry $e -To $w.T
                }
                Report -Status 'Done' -Text ('dgVoodoo2 installed: MS\' + $sub + '\' + $wrapName + ', dgVoodoo.conf, dgVoodooCpl.exe.')
            }
            finally { $z.Dispose() }
        }
        catch { Report -Status 'Fail' -Text 'dgVoodoo2 install failed.' -Detail $_.Exception.Message }
    }
    else {
        Report -Status 'Fail' -Text 'dgVoodoo2 is not available; the Direct3D 8/9 path cannot work without it.'
    }

    # Settings from the README's table. Watermark off by default (-DgVoodooWatermark keeps it).
    $ct = Read-TextSafe $confPath
    if ($ct) {
        $wm = if ($DgVoodooWatermark) { 'true' } else { 'false' }
        $new = $ct
        $new = Set-ConfValue -Text $new -Section 'General' -Key 'OutputAPI' -Value 'd3d11_fl11_0'
        $new = Set-ConfValue -Text $new -Section 'DirectX' -Key 'DisableAndPassThru' -Value 'false'
        $new = Set-ConfValue -Text $new -Section 'DirectX' -Key 'VideoCard' -Value 'internal3D'
        $new = Set-ConfValue -Text $new -Section 'DirectX' -Key 'VRAM' -Value '1GB'
        $new = Set-ConfValue -Text $new -Section 'DirectX' -Key 'dgVoodooWatermark' -Value $wm
        if ($new.TrimEnd() -ne $ct.TrimEnd()) {
            Write-TextTracked -Path $confPath -Text $new
            Report -Status 'Done' -Text ('dgVoodoo.conf: DisableAndPassThru=false, VideoCard=internal3D, VRAM=1GB, OutputAPI=d3d11_fl11_0, dgVoodooWatermark=' + $wm + '.')
        }
        else { Report -Status 'Ok' -Text 'dgVoodoo.conf already has the required values.' }
    }
}

# ---------------------------------------------------------------------------------------
# 6. The feeder
# ---------------------------------------------------------------------------------------

Write-Section 'DLSS5-Feeder'

$addonName = if ($is32) { 'dlss5-feed.addon32' } else { 'dlss5-feed.addon64' }
$wrongAddon = if ($is32) { 'dlss5-feed.addon64' } else { 'dlss5-feed.addon32' }
$verifyScript = $null
$feederVersion = $null

try {
    $z = Open-Zip $feederPath
    try {
        $e = Find-ZipEntry $z ('(?i)(^|/)' + [regex]::Escape($addonName) + '$')
        if (-not $e) { throw ($addonName + ' not found in ' + $feederPath) }
        Expand-ZipEntry -Entry $e -To (Join-Safe $gameDir $addonName)

        $fx = Find-ZipEntry $z '(?i)(^|/)DLSS5_Feed\.fx$'
        if (-not $fx) { throw 'DLSS5_Feed.fx not found in the release' }
        Expand-ZipEntry -Entry $fx -To (Join-Safe $shaderDir 'DLSS5_Feed.fx')

        $m = [regex]::Match([IO.Path]::GetFileName($feederPath), 'DLSS5-Feeder-(.+)\.zip$')
        if ($m.Success) { $feederVersion = $m.Groups[1].Value }
        Report -Status 'Done' -Text ($addonName + ' and reshade-shaders\Shaders\DLSS5_Feed.fx installed' + $(if ($feederVersion) { ' (' + $feederVersion + ')' } else { '' }) + '.')

        if ($is32) {
            $h = Find-ZipEntry $z '(?i)(^|/)dlss5-feed-host64\.exe$'
            if (-not $h) { throw 'host64/dlss5-feed-host64.exe not found in the release' }
            Expand-ZipEntry -Entry $h -To (Join-Safe $hostDir 'dlss5-feed-host64.exe')
            Report -Status 'Done' -Text 'host64\dlss5-feed-host64.exe installed (same release as the add-on).'
        }

        if ($isVulkan) {
            $layerSub = if ($is32) { 'layer-x86' } else { 'layer-x64' }
            $n = 0
            foreach ($le in $z.Entries) {
                $ln = $le.FullName -replace '\\', '/'
                if ($ln -match ('(?i)^' + $layerSub + '/(.+)$') -and -not $ln.EndsWith('/')) {
                    Expand-ZipEntry -Entry $le -To (Join-Safe $gameDir ('layer\' + $Matches[1]))
                    $n++
                }
            }
            if ($n -gt 0) { Report -Status 'Done' -Text ('Vulkan fallback layer copied to layer\ (' + $n + ' files).') -Detail 'Only needed if dlss5-feed.log says the Vulkan interop entry points are missing: launch through layer\run-with-feed-layer*.bat.' }
        }

        $v = Find-ZipEntry $z '(?i)(^|/)Verify-DLSS5Feeder\.ps1$'
        if ($v) { $verifyScript = Join-Safe $gameDir 'Verify-DLSS5Feeder.ps1'; Expand-ZipEntry -Entry $v -To $verifyScript }
    }
    finally { $z.Dispose() }
}
catch { Report -Status 'Fail' -Text 'Feeder files could not be installed.' -Detail $_.Exception.Message }

$stray = Find-FileIn $gameDir $wrongAddon
if ($stray) {
    try { Rename-Item -LiteralPath $stray -NewName ($wrongAddon + '.disabled-by-installer') -Force; Report -Status 'Done' -Text ($wrongAddon + ' (wrong architecture) renamed to .disabled-by-installer.') }
    catch { Report -Status 'Warn' -Text ($wrongAddon + ' is also present and is for the other architecture; remove it.') }
}

# Framework headers
$missing = @()
foreach ($h in @('ReShade.fxh', 'ReShadeUI.fxh', 'DrawText.fxh')) {
    if (Find-FileUnder $shaderDir $h) { continue }
    $src = $headerPaths[$h]
    if ($src) { try { Copy-Tracked -From $src -To (Join-Safe $shaderDir $h) } catch { $missing += $h } }
    else { $missing += $h }
}
if ($missing.Count -eq 0) { Report -Status 'Ok' -Text 'ReShade framework headers in place (ReShade.fxh, ReShadeUI.fxh, DrawText.fxh).' }
else { Report -Status 'Fail' -Text ('Missing framework header(s): ' + ($missing -join ', ')) -Detail 'Copy them from https://github.com/crosire/reshade-shaders/tree/slim/Shaders into reshade-shaders\Shaders\.' }

# ---------------------------------------------------------------------------------------
# 7. Motion vectors: LumeniteFX
# ---------------------------------------------------------------------------------------

Write-Section 'Motion vectors (LumeniteFX)'

$providerFx = if ($MvProvider -eq 4) { 'lumenite_QuantMotion.fx' } else { 'lumenite_Kernel.fx' }
$providerTechnique = if ($MvProvider -eq 4) { 'Lumenite_QuantMotion@lumenite_QuantMotion.fx' } else { 'Lumenite_Kernel@lumenite_Kernel.fx' }

if ($lumenitePath) {
    try {
        $z = Open-Zip $lumenitePath
        try {
            $n = 0
            foreach ($e in $z.Entries) {
                $en = $e.FullName -replace '\\', '/'
                if ($en.EndsWith('/')) { continue }
                if ($en -match '(?i)(^|/)Shaders/(lumenite_[^/]+\.fx)$') { Expand-ZipEntry -Entry $e -To (Join-Safe $shaderDir $Matches[2]); $n++ }
                elseif ($en -match '(?i)(^|/)Shaders/include/([^/]+\.fxh)$') { Expand-ZipEntry -Entry $e -To (Join-Safe $shaderDir ('include\' + $Matches[2])); $n++ }
                elseif ($en -match '(?i)(^|/)Textures/([^/]+)$') { Expand-ZipEntry -Entry $e -To (Join-Safe $textureDir $Matches[2]); $n++ }
            }
            if ($n -eq 0) { throw 'no Shaders\ or Textures\ entries found in the LumeniteFX zip' }
            Report -Status 'Done' -Text ('LumeniteFX installed (' + $n + ' files into reshade-shaders\Shaders\ and Textures\).')
        }
        finally { $z.Dispose() }
    }
    catch { Report -Status 'Fail' -Text 'LumeniteFX could not be installed.' -Detail $_.Exception.Message }
}
else {
    Report -Status 'Fail' -Text 'LumeniteFX is not available.' -Detail 'https://github.com/umar-afzaal/LumeniteFX (Code > Download ZIP), then -LumeniteZip <file>.'
}
if (-not (Find-FileUnder $shaderDir $providerFx)) { Report -Status 'Fail' -Text ($providerFx + ' is not installed; DLSS5_MV_PROVIDER=' + $MvProvider + ' has nothing to read.') }

# ---------------------------------------------------------------------------------------
# 8. Neural consumer and NVIDIA runtimes
# ---------------------------------------------------------------------------------------

Write-Section ('Neural consumer (' + $consumerWhere + ')')

New-DirSafe $consumerDir

# Files that must not coexist with the chosen consumer.
function Disable-Conflict
{
    param([string] $Path, [string] $Why)
    if (-not (Test-FileHere $Path)) { return }
    $n = [IO.Path]::GetFileName($Path)
    if ($Yes -or (Confirm-Step ('Rename ' + $n + ' to .disabled-by-installer? (' + $Why + ')'))) {
        try { Rename-Item -LiteralPath $Path -NewName ($n + '.disabled-by-installer') -Force; Report -Status 'Done' -Text ($n + ' renamed to .disabled-by-installer.') -Detail $Why }
        catch { Report -Status 'Warn' -Text ($n + ' could not be renamed.') -Detail $_.Exception.Message }
    }
    else { Report -Status 'Warn' -Text ($n + ' left in place.') -Detail $Why -Manual ('Remove ' + $Path + ' before playing.') }
}

Disable-Conflict -Path (Find-FileIn $consumerDir 'dlss5-dx11-bridge.addon64') -Why 'the DX11 bridge must never be combined with DLSS5-Feeder'
if ($is32) {
    foreach ($n in @('deep-fried-chicken.addon64', 'renodx-dlss5.addon64', 'alexs-toolkit.addon64')) {
        Disable-Conflict -Path (Find-FileIn $gameDir $n) -Why 'a 64-bit add-on beside a 32-bit exe is never loaded; the consumer belongs in host64\'
    }
}

if ($Consumer -eq 'DFC') {
    Disable-Conflict -Path (Find-FileIn $consumerDir 'renodx-dlss5.addon64') -Why 'Deep Fried Chicken stays inert while a RenoDX neural provider is loaded'
    Disable-Conflict -Path (Find-FileIn $consumerDir 'alexs-toolkit.addon64') -Why 'a third interposer on the same NGX module; Chicken''s docs ask for it to be removed'

    if ($dfcPath) {
        $dfcFiles = @('deep-fried-chicken.addon64', 'deep-fried-chicken-nvngx.dll', 'deep-fried-chicken.cfg')
        $attempt = 0
        $dfcOk = $false
        while ($attempt -lt 2 -and -not $dfcOk) {
            $attempt++
            $sums = @{}
            $failed = $null
            $dfcVersion = $null
            try {
                $z = Open-Zip $dfcPath
                try {
                    $se = Find-ZipEntry $z '(?i)(^|/)SHA256SUMS\.txt$'
                    if ($se) {
                        $st = Read-ZipText $se
                        $vm = [regex]::Match($st, 'Deep Fried Chicken (\S+)')
                        if ($vm.Success) { $dfcVersion = $vm.Groups[1].Value }
                        foreach ($sm in [regex]::Matches($st, '(?m)^\s*([0-9A-Fa-f]{64})\s+(\S+)\s*$')) { $sums[$sm.Groups[2].Value.ToLowerInvariant()] = $sm.Groups[1].Value.ToUpperInvariant() }
                    }
                    foreach ($f in $dfcFiles) {
                        $e = Find-ZipEntry $z ('(?i)(^|/)' + [regex]::Escape($f) + '$')
                        if (-not $e) { throw ($f + ' not found in the Deep Fried Chicken zip') }
                        $to = Join-Safe $consumerDir $f
                        # Keep an existing cfg: it holds the user's settings. Only add it when absent.
                        if ($f -eq 'deep-fried-chicken.cfg' -and (Test-FileHere $to) -and -not $Force) { continue }
                        Expand-ZipEntry -Entry $e -To $to
                    }
                }
                finally { $z.Dispose() }

                # Did the files survive? Defender removes the add-on within seconds of extraction.
                Start-Sleep -Milliseconds 1500
                foreach ($f in @('deep-fried-chicken.addon64', 'deep-fried-chicken-nvngx.dll')) {
                    $to = Join-Safe $consumerDir $f
                    if (-not (Test-FileHere $to)) { $failed = $to; break }
                    if ($sums.ContainsKey($f)) {
                        $h = Get-Sha256 $to
                        if ($h -and $h -ne $sums[$f]) { throw ($f + ' does not match the SHA-256 in the zip''s SHA256SUMS.txt') }
                    }
                }
            }
            catch {
                $msg = $_.Exception.Message
                if ($msg -match '(?i)virus|potentially unwanted|0x800700E1') { $failed = Join-Safe $consumerDir 'deep-fried-chicken.addon64' }
                else { Report -Status 'Fail' -Text 'Deep Fried Chicken could not be installed.' -Detail $msg; break }
            }

            if (-not $failed) {
                $dfcOk = $true
                $t = 'Deep Fried Chicken'
                if ($dfcVersion) { $t += ' ' + $dfcVersion }
                $t += ' installed (add-on, NGX bridge, cfg)'
                if ($sums.Count -gt 0) { $t += ', SHA-256 verified' }
                Report -Status 'Done' -Text ($t + '.') -Detail ('in ' + $consumerDir)
                break
            }

            # Removed. Confirm with Defender's own log, then ask.
            $det = Get-DefenderDetection $failed
            $why = 'It removed ' + $failed
            if ($det) { $why += ' (Defender log: ' + $det.InitialDetectionTime.ToString('HH:mm:ss') + ', threat id ' + $det.ThreatID + ')' } else { $why += ' right after extraction' }
            $why += '.'
            if ($attempt -ge 2) {
                Report -Status 'Fail' -Text 'Deep Fried Chicken was removed again after the exclusion was added.' `
                       -Detail 'Check Windows Security > Protection history, restore the quarantined file, and confirm the exclusion covers the folder above.'
                break
            }
            $ok = Request-DefenderExclusion -Paths @($gameDir, $script:Cache) -Because $why
            if (-not $ok) { break }
            Report -Status 'Info' -Text 'Retrying the Deep Fried Chicken extraction.'
        }
    }
}
else {
    Disable-Conflict -Path (Find-FileIn $consumerDir 'deep-fried-chicken.addon64') -Why 'exactly one neural consumer: you chose RenoDX'
    Disable-Conflict -Path (Find-FileIn $consumerDir 'deep-fried-chicken-nvngx.dll') -Why 'Chicken''s private NGX bridge has no business beside the RenoDX add-on'

    if ($renoPath) {
        $to = Join-Safe $consumerDir 'renodx-dlss5.addon64'
        $attempt = 0
        while ($attempt -lt 2) {
            $attempt++
            $failed = $null
            try {
                Copy-Tracked -From $renoPath -To $to
                # Same antivirus story as Chicken: an NGX interposer looks like a hooking tool.
                Start-Sleep -Milliseconds 1000
                if (-not (Test-FileHere $to)) { $failed = $to }
            }
            catch {
                $msg = $_.Exception.Message
                if ($msg -match '(?i)virus|potentially unwanted|0x800700E1') { $failed = $to }
                else { Report -Status 'Fail' -Text 'renodx-dlss5.addon64 could not be copied.' -Detail $msg; break }
            }

            if (-not $failed) {
                # Krish's builds all report the same file version resource, so the generation is
                # only readable as the standalone banner literal the add-on prints at run time
                # (the feeder reads the same string).
                $banner = $null
                if (Get-BinaryMarker -Path $to -Pattern 'RenoDX DLSS5 Generic') {
                    $banner = Get-BinaryMarker -Path $to -Pattern "\x00(v\d+\.\d+)\x00"
                }
                $t = 'renodx-dlss5.addon64 installed'
                if ($banner) { $t += ' (' + $banner + ')' }
                Report -Status 'Done' -Text ($t + '.') -Detail ('in ' + $consumerWhere)
                break
            }

            $det = Get-DefenderDetection $failed
            $why = 'It removed ' + $failed
            if ($det) { $why += ' (Defender log: ' + $det.InitialDetectionTime.ToString('HH:mm:ss') + ', threat id ' + $det.ThreatID + ')' } else { $why += ' right after it was copied in' }
            $why += '.'
            if ($attempt -ge 2) {
                Report -Status 'Fail' -Text 'renodx-dlss5.addon64 was removed again after the exclusion was added.' `
                       -Detail 'Check Windows Security > Protection history, restore the quarantined file, and confirm the exclusion covers the folder above.'
                break
            }
            if (-not (Request-DefenderExclusion -Paths @($gameDir, $script:Cache) -Because $why)) { break }
            Report -Status 'Info' -Text 'Retrying the RenoDX add-on copy.'
        }
    }
}

# NVIDIA runtimes
foreach ($pair in @(@{ N = 'nvngx_dlssnr.dll'; P = $dlssNrPath }, @{ N = 'nvngx_dlss.dll'; P = $dlssPath })) {
    $to = Join-Safe $consumerDir $pair.N
    if (-not $pair.P) { continue }
    if ((Test-FileHere $to) -and -not $Force) {
        $have = Get-Sha256 $to
        $new = Get-Sha256 $pair.P
        if ($have -and $new -and $have -eq $new) { Report -Status 'Ok' -Text ($pair.N + ' already present (identical).'); continue }
        $vHave = Get-FileVersionSafe $to; $vNew = Get-FileVersionSafe $pair.P
        Report -Status 'Info' -Text ($pair.N + ': replacing ' + $vHave + ' with ' + $vNew + '.')
    }
    try { Copy-Tracked -From $pair.P -To $to; Report -Status 'Done' -Text ($pair.N + ' ' + (Get-FileVersionSafe $to) + ' installed.') -Detail ('in ' + $consumerWhere) }
    catch { Report -Status 'Fail' -Text ($pair.N + ' could not be copied.') -Detail $_.Exception.Message }
}

# ---------------------------------------------------------------------------------------
# 9. The d3dcompiler_47.dll trap
# ---------------------------------------------------------------------------------------

Write-Section 'd3dcompiler_47.dll'
$dcDirs = @($gameDir)
if ($is32) { $dcDirs += $hostDir }
$dcFound = $false
foreach ($d in $dcDirs) {
    $p = Find-FileIn $d 'd3dcompiler_47.dll'
    if (-not $p) { continue }
    $dcFound = $true
    $v = Get-FileVersionSafe $p
    if ($v -match '^6\.3\.') {
        try {
            Rename-Item -LiteralPath $p -NewName 'd3dcompiler_47.dll.disabled-by-installer' -Force
            Report -Status 'Done' -Text ('Windows 8.1-era d3dcompiler_47.dll (' + $v + ') renamed to .disabled-by-installer.') -Detail ($p + "`nIt cannot compile the cs_5_1 neural pass; System32's copy is used instead, which is fine.")
        }
        catch { Report -Status 'Fail' -Text ('Old d3dcompiler_47.dll (' + $v + ') could not be renamed.') -Detail $p -Manual ('Rename or delete ' + $p) }
    }
    else { Report -Status 'Ok' -Text ('d3dcompiler_47.dll ' + $v + ' next to the exe is new enough.') -Detail $p }
}
if (-not $dcFound) { Report -Status 'Ok' -Text 'No local d3dcompiler_47.dll; System32''s copy is used.' }

# ---------------------------------------------------------------------------------------
# 10. ReShade.ini and ReShadePreset.ini
# ---------------------------------------------------------------------------------------

Write-Section 'Configuration files'

$iniTemplate = @"
[ADDON]
AddonPath=.\

[DEPTH]
DepthCopyBeforeClears=0

[GENERAL]
EffectSearchPaths=.\reshade-shaders\Shaders\**
TextureSearchPaths=.\reshade-shaders\Textures\**
IntermediateCachePath=
NoDebugInfo=1
NoEffectCache=0
NoReloadOnInit=0
PerformanceMode=0
PreprocessorDefinitions=
PresetPath=.\ReShadePreset.ini
PresetShortcutKeys=
PresetShortcutPaths=
PresetTransitionDuration=1000
SkipLoadingDisabledEffects=0
StartupPresetPath=

[INPUT]
ForceShortcutModifiers=1
InputProcessing=2
KeyEffects=222,0,0,0
KeyOverlay=36,0,0,0
KeyReload=0,0,0,0
KeyScreenshot=220,0,0,0

[OVERLAY]
TutorialProgress=4
"@

$iniPath = Join-Safe $gameDir 'ReShade.ini'
$depthClears = if ($isDgV) { '1' } else { '0' }   # UE3-era D3D9 games clear depth mid-frame
$existingIni = Read-TextSafe $iniPath
if ($existingIni -and -not $Force) {
    $new = $existingIni
    $new = Set-IniKey -Text $new -Section 'ADDON' -Key 'AddonPath' -Value '.\'
    $new = Set-IniKey -Text $new -Section 'GENERAL' -Key 'EffectSearchPaths' -Value '.\reshade-shaders\Shaders\**'
    $new = Set-IniKey -Text $new -Section 'GENERAL' -Key 'TextureSearchPaths' -Value '.\reshade-shaders\Textures\**'
    if (-not (Get-IniKey -Text $new -Section 'GENERAL' -Key 'PresetPath')) { $new = Set-IniKey -Text $new -Section 'GENERAL' -Key 'PresetPath' -Value '.\ReShadePreset.ini' }
    if ($isDgV) { $new = Set-IniKey -Text $new -Section 'DEPTH' -Key 'DepthCopyBeforeClears' -Value $depthClears }
    if ($new.TrimEnd() -ne $existingIni.TrimEnd()) {
        $bak = Backup-File $iniPath
        Write-TextTracked -Path $iniPath -Text $new
        Report -Status 'Done' -Text 'ReShade.ini: existing file kept, [ADDON]/[GENERAL] keys merged in.' -Detail ('backup: ' + $bak)
    }
    else { Report -Status 'Ok' -Text 'ReShade.ini already has the required keys.' }
}
else {
    $bak = $null
    if ($existingIni) { $bak = Backup-File $iniPath }
    $t = $iniTemplate -replace 'DepthCopyBeforeClears=0', ('DepthCopyBeforeClears=' + $depthClears)
    Write-TextTracked -Path $iniPath -Text $t
    Report -Status 'Done' -Text 'ReShade.ini written from the template.' -Detail $(if ($bak) { 'previous file backed up: ' + $bak } else { '' })
}

# ReShadePreset.ini: the provider must be enabled and run BEFORE DLSS5_Feed, and the
# provider choice lives in the effect's own section (not in ReShade.ini).
$presetPath = Join-Safe $gameDir 'ReShadePreset.ini'
$presetPathFromIni = Get-IniKey -Text (Read-TextSafe $iniPath) -Section 'GENERAL' -Key 'PresetPath'
if ($presetPathFromIni -and -not [IO.Path]::IsPathRooted($presetPathFromIni)) { $presetPath = [IO.Path]::GetFullPath((Join-Safe $gameDir $presetPathFromIni)) }
elseif ($presetPathFromIni) { $presetPath = $presetPathFromIni }

$feedTechnique = 'DLSS5_Feed@DLSS5_Feed.fx'
$existingPreset = Read-TextSafe $presetPath
if ($existingPreset -and -not $Force) {
    $new = $existingPreset
    foreach ($key in @('Techniques', 'TechniqueSorting')) {
        $cur = Get-IniKey -Text $new -Section '' -Key $key
        $list = @()
        if ($cur) { $list = @($cur -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
        $list = @($list | Where-Object { $_ -ine $providerTechnique -and $_ -ine $feedTechnique -and $_ -notmatch '(?i)^DLSS5_Feed_Debug@' })
        $list += $providerTechnique
        $list += $feedTechnique
        if ($key -eq 'TechniqueSorting') { $list += 'DLSS5_Feed_Debug@DLSS5_Feed.fx' }
        $new = Set-IniKey -Text $new -Section '' -Key $key -Value ($list -join ',')
    }
    $defs = Get-IniKey -Text $new -Section 'DLSS5_Feed.fx' -Key 'PreprocessorDefinitions'
    if ($defs) {
        $parts = @($defs -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -and $_ -notmatch '(?i)^DLSS5_MV_PROVIDER\s*=' })
        $parts += ('DLSS5_MV_PROVIDER=' + $MvProvider)
        $defs = $parts -join ','
    }
    else { $defs = 'DLSS5_MV_PROVIDER=' + $MvProvider }
    $new = Set-IniKey -Text $new -Section 'DLSS5_Feed.fx' -Key 'PreprocessorDefinitions' -Value $defs
    if ($new.TrimEnd() -ne $existingPreset.TrimEnd()) {
        $bak = Backup-File $presetPath
        Write-TextTracked -Path $presetPath -Text $new
        Report -Status 'Done' -Text ('ReShadePreset.ini: existing preset kept; ' + $providerTechnique.Split('@')[0] + ' and DLSS5_Feed enabled in that order, DLSS5_MV_PROVIDER=' + $MvProvider + '.') -Detail ('backup: ' + $bak)
    }
    else { Report -Status 'Ok' -Text 'ReShadePreset.ini already correct.' }
}
else {
    $bak = $null
    if ($existingPreset) { $bak = Backup-File $presetPath }
    $preset = 'Techniques=' + $providerTechnique + ',' + $feedTechnique + "`r`n" +
              'TechniqueSorting=' + $providerTechnique + ',' + $feedTechnique + ',DLSS5_Feed_Debug@DLSS5_Feed.fx' + "`r`n`r`n" +
              '[DLSS5_Feed.fx]' + "`r`n" + 'DEBUG_VIEW=0' + "`r`n" + 'MV_SCALE=1.000000' + "`r`n" + 'MV_SIGN=1.000000,1.000000' + "`r`n" +
              'PreprocessorDefinitions=DLSS5_MV_PROVIDER=' + $MvProvider + "`r`n"
    Write-TextTracked -Path $presetPath -Text $preset
    Report -Status 'Done' -Text ('ReShadePreset.ini written: ' + $providerTechnique.Split('@')[0] + ' then DLSS5_Feed, DLSS5_MV_PROVIDER=' + $MvProvider + '.') -Detail $(if ($bak) { 'previous file backed up: ' + $bak } else { '' })
}

# A DLSS5_MV_PROVIDER in ReShade.ini does nothing and only confuses; say so.
$riNow = Read-TextSafe $iniPath
if ($riNow -and $riNow -match '(?i)DLSS5_MV_PROVIDER') {
    Report -Status 'Warn' -Text 'ReShade.ini also mentions DLSS5_MV_PROVIDER; that is a per-effect key and is ignored there.' -Detail 'Only the [DLSS5_Feed.fx] section of ReShadePreset.ini counts. Harmless, but remove it to avoid confusion.'
}

# host64\ReShade.ini: minimal, and deliberately without AddonPath (the host loads its own folder's add-ons).
if ($is32) {
    $hostIni = Join-Safe $hostDir 'ReShade.ini'
    if (-not (Test-FileHere $hostIni)) {
        Write-TextTracked -Path $hostIni -Text ("[GENERAL]`r`nEffectSearchPaths=.\`r`nTextureSearchPaths=.\`r`n")
        Report -Status 'Done' -Text 'host64\ReShade.ini written (minimal).'
    }
    else { Report -Status 'Ok' -Text 'host64\ReShade.ini already present, kept.' }
}

# ---------------------------------------------------------------------------------------
# 11. Mark-of-the-web off everything we wrote
# ---------------------------------------------------------------------------------------

$unblocked = 0
foreach ($p in $script:Changed) {
    try { if (Test-FileHere $p) { Unblock-File -LiteralPath $p -ErrorAction SilentlyContinue; $unblocked++ } } catch { }
}

# ---------------------------------------------------------------------------------------
# Summary, verification, next steps
# ---------------------------------------------------------------------------------------

Write-Host ''
Write-Chunk ('  ' + ([string][char]0x2500) * 68) 'Green'
Write-Chunk '  ' $null -NoNewline
Write-Chunk ($script:CountDone.ToString() + ' installed') 'Green' -NoNewline
Write-Chunk '   ' $null -NoNewline
Write-Chunk ($script:CountWarn.ToString() + ' warning' + $(if ($script:CountWarn -eq 1) { '' } else { 's' })) 'Yellow' -NoNewline
Write-Chunk '   ' $null -NoNewline
Write-Chunk ($script:CountFail.ToString() + ' failure' + $(if ($script:CountFail -eq 1) { '' } else { 's' })) 'Red'

if ($script:Manual.Count -gt 0) {
    Write-Host ''
    Write-Chunk '  Still to do by hand:' 'White'
    $i = 1
    foreach ($a in $script:Manual) {
        $first = $true
        foreach ($line in ($a -split "`n")) {
            if (-not $line.Trim()) { continue }
            if ($first) { Write-Chunk ('   ' + $i + '. ' + $line.Trim()) 'DarkYellow'; $first = $false }
            else { Write-Chunk ('      ' + $line.Trim()) 'DarkYellow' }
        }
        $i++
    }
}

if (-not $NoVerify -and $verifyScript -and (Test-FileHere $verifyScript)) {
    Write-Host ''
    Write-Chunk '  Running Verify-DLSS5Feeder.ps1 on the result ...' 'White'
    try {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $verifyScript -GamePath $exePath -NoPause
        $vc = $LASTEXITCODE
        if ($vc -ne 0) { Report -Status 'Warn' -Text 'The verifier reports failures; see above.' }
    }
    catch { Report -Status 'Warn' -Text 'Could not run the verifier.' -Detail $_.Exception.Message }
}

Write-Host ''
Write-Chunk '  Next, in the game:' 'White'
$steps = @()
$steps += 'Launch it. Press Home for the ReShade overlay and check there are no compile errors.'
$steps += ('Both techniques should already be ticked, in this order: ' + $providerTechnique.Split('@')[0] + ' above DLSS 5 Feed.')
if ($Consumer -eq 'DFC') {
    if ($is32) { $steps += 'Open the ReShade overlay > Add-ons > DLSS 5 Feed and press "Show the DLSS 5 panel in-game" to reach the Deep Fried Chicken tab (it lives in the host64 helper).' }
    else { $steps += 'Turn on neural rendering in the Deep Fried Chicken tab of the overlay.' }
    $steps += 'On its first armed run Chicken registers itself for early load in ReShade.ini and asks for one more full restart. Do that restart.'
}
else {
    $steps += 'Turn on neural rendering in the DLSS 5 Neural Rendering add-on panel.'
}
$steps += 'Turn the game''s MSAA/SSAA off.'
if ($isDgV -and -not $DgVoodooWatermark) { $steps += 'dgVoodoo''s watermark is off. If nothing seems to happen, re-run with -DgVoodooWatermark to confirm dgVoodoo is active at all.' }
if ($isVulkan) { $steps += 'If dlss5-feed.log says the Vulkan interop entry points are missing, launch through layer\run-with-feed-layer' + $(if ($is32) { '32' } else { '' }) + '.bat "<path to exe>" instead.' }
$steps += 'Then check dlss5-feed.log next to the exe for "feature ready", "frame N delivered", and re-run Verify-DLSS5Feeder.ps1 for a runtime verdict.'
$i = 1
foreach ($s in $steps) { Write-Chunk ('   ' + $i + '. ' + $s) 'Gray'; $i++ }

Write-Host ''
if ($script:CountFail -gt 0) { Exit-Installer 1 }
Exit-Installer 0
