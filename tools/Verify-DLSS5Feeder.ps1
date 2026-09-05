<#
.SYNOPSIS
    Verifies a DLSS5-Feeder installation in a game folder and reports what is right and
    what is wrong.

.DESCRIPTION
    DLSS5-Feeder is a ReShade add-on that manufactures a synthetic NVIDIA DLSS contract for
    games that have no DLSS of their own, so that a separate "neural consumer" add-on (Deep
    Fried Chicken, or the RenoDX DLSS 5 add-on) can perform neural rendering on top of it.

    Getting that working requires a fiddly, mutually-consistent set of files, and most of the
    ways to get it wrong fail *silently* -- the game launches, ReShade loads, frames are
    delivered, and neural rendering simply does nothing. This script inspects a game folder
    and checks, read-only:

      1. The game's architecture (parsed straight out of the PE header) and render API.
      2. The ReShade install -- local DLL or machine-wide Vulkan layer -- and its version.
      3. The feeder's own files: the add-on, the 32-bit host helper, the shader and its
         framework headers.
      4. The neural consumer: exactly one must be present, and never a conflicting second.
      5. The NVIDIA NGX runtimes (nvngx_dlssnr.dll, nvngx_dlss.dll).
      6. The d3dcompiler_47.dll trap: a Windows 8.1-SDK-era copy in the game folder silently
         kills the neural pass.
      7. The GPU (DLSS needs an RTX card).
      8. A short digest of the feeder's own logs, if it has run.
      9. The motion-vector provider, which is a per-effect ReShade key that people routinely
         set in the wrong file.

    The script NEVER writes, moves or deletes anything. It exits with code 1 if any check
    failed, 0 otherwise, so it can be used from another script.

.PARAMETER GamePath
    The game folder to check, or the path to the game's .exe (the folder is then taken from
    it). Defaults to the current directory.

.PARAMETER Exe
    Overrides the automatic choice of the game executable. Useful when a folder holds several
    (for example DOOM's DOOMx64.exe and DOOMx64vk.exe). May be a bare file name or a path.

.PARAMETER Quiet
    Print only failures and the closing summary.

.EXAMPLE
    .\Verify-DLSS5Feeder.ps1 -GamePath "G:\Games\Dusk"

    Check a 64-bit Direct3D 11 install.

.EXAMPLE
    .\Verify-DLSS5Feeder.ps1 "E:\SteamLibrary\steamapps\common\DOOM" -Exe DOOMx64vk.exe

    Check a Vulkan install, pointing the script at the Vulkan executable rather than letting
    it guess.

.EXAMPLE
    .\Verify-DLSS5Feeder.ps1

    Check the current directory.

.NOTES
    Windows PowerShell 5.1 compatible. Read-only. See DEPLOY-DEV.md in the DLSS5-Feeder repo
    for the full install runbook that these checks are derived from.
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string] $GamePath,

    [string] $Exe,

    [switch] $Quiet,

    # Skip the "Press Enter to exit" pause at the end. The pause exists for the
    # right-click > "Run with PowerShell" case, where the window would otherwise close
    # before the output can be read; -Quiet implies it.
    [switch] $NoPause
)

function Exit-Verifier {
    param([int] $Code)
    if (-not $Quiet -and -not $NoPause) {
        Write-Host ''
        try { [void](Read-Host '  Press Enter to exit') } catch { }
    }
    exit $Code
}

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------------------
# Output plumbing
# ---------------------------------------------------------------------------------------

$script:CountOk   = 0
$script:CountWarn = 0
$script:CountFail = 0
# DLSS5_MV_PROVIDER as the add-on itself resolved it in the last run, out of dlss5-feed.log.
# Better evidence than any ini, because it is read after ReShade has done the overriding.
$script:FeedLogProvider = $null
$script:Actions   = New-Object System.Collections.ArrayList

# The first NVIDIA driver that ships DLSS 5 neural rendering at all. Below it NGX answers
# FeatureNotSupported (0xBAD00001) and nothing here works, so it is a hard failure -- see
# issue #47, where a machine on 596.36 passed this script with 0 failures.
$kMinDriver       = '616.56'
$kMinDriverDigits = 61656

# Does this host support colour at all? A transcript, a redirected stream or an exotic host
# may not. Probe once and fall back to plain text rather than throwing per line.
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
        # A host that accepted the probe but rejects this colour: never let painting fail a run.
        $script:UseColour = $false
        Write-Host $Text -NoNewline:$NoNewline
    }
}

function Write-Banner
{
    if ($Quiet) { return }

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
        @{ Mark = '    ' + ($lwr * 2) + ($blk * 5); Shade = 'Green';   Text = '   DLSS5-Feeder'; TextColour = 'White' },
        @{ Mark = '   ' + ($blk * 6) + ($upr * 2); Shade = 'Green';    Text = '   Neural rendering bridge for ReShade'; TextColour = 'DarkGray' },
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
    if ($Quiet) { return }
    Write-Host ''
    Write-Chunk ('  ' + [char]0x2500 + [char]0x2500 + ' ') 'Green' -NoNewline
    Write-Chunk $Title 'White' -NoNewline
    $pad = 62 - $Title.Length
    if ($pad -lt 1) { $pad = 1 }
    Write-Chunk (' ' + ([string][char]0x2500) * $pad) 'Green'
}

# The one place a check result is recorded. $Status is Ok / Warn / Fail / Na.
function Report
{
    param(
        [ValidateSet('Ok', 'Warn', 'Fail', 'Na')]
        [string] $Status,
        [string] $Text,
        [string] $Detail,
        [string] $Action
    )

    switch ($Status) {
        'Ok'   { $glyph = '[ OK ]'; $colour = 'Green';    $script:CountOk++ }
        'Warn' { $glyph = '[WARN]'; $colour = 'Yellow';   $script:CountWarn++ }
        'Fail' { $glyph = '[FAIL]'; $colour = 'Red';      $script:CountFail++ }
        'Na'   { $glyph = '[ -- ]'; $colour = 'DarkGray' }
    }

    if ($Status -eq 'Fail' -and $Action) { $null = $script:Actions.Add($Action) }

    if ($Quiet -and $Status -ne 'Fail') { return }

    Write-Chunk ('  ' + $glyph + ' ') $colour -NoNewline
    if ($Status -eq 'Na') { Write-Chunk $Text 'DarkGray' } else { Write-Host $Text }
    if ($Detail) {
        foreach ($line in ($Detail -split "`n")) {
            if ($line.Trim()) { Write-Chunk ('         ' + $line.Trim()) 'DarkGray' }
        }
    }
    if ($Status -eq 'Fail' -and $Action) {
        $first = $true
        foreach ($line in ($Action -split "`n")) {
            if (-not $line.Trim()) { continue }
            if ($first) { Write-Chunk ('         ' + [char]0x2192 + ' ' + $line.Trim()) 'DarkYellow'; $first = $false }
            else { Write-Chunk ('           ' + $line.Trim()) 'DarkYellow' }
        }
    }
}

# ---------------------------------------------------------------------------------------
# Small, defensive helpers. Every one of these returns $null rather than throwing.
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

# Case-insensitive lookup of a file in a folder; returns the real path or $null. Used so the
# script does not care whether a game shipped D3D9.dll or d3d9.dll, ReShade.ini or reshade.ini.
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

# Case-insensitive recursive search under a folder. ReShade's EffectSearchPaths normally ends
# in "**", so a header sitting in Shaders\CrosireMaster\ is found by the compiler and must be
# treated as present here too.
function Find-FileUnder
{
    param([string] $Dir, [string] $Name)
    if (-not (Test-DirHere $Dir)) { return $null }
    try {
        $hit = Get-ChildItem -LiteralPath $Dir -File -Filter $Name -Recurse -ErrorAction SilentlyContinue |
               Select-Object -First 1
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
        if ($vi -and $vi.FileVersion) {
            # Some vendors (NVIDIA's NGX DLLs among them) write "310,8,0,0".
            return ($vi.FileVersion.Trim() -replace '\s*,\s*', '.')
        }
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

# Parse the PE header ourselves: no shelling out, and it works on a file a running game holds
# open (FileShare ReadWrite).
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

        if ($br.ReadUInt16() -ne 0x5A4D) { return $null }        # 'MZ'
        $fs.Position = 0x3C
        $peOff = $br.ReadInt32()
        if ($peOff -le 0 -or ($peOff + 24) -ge $fs.Length) { return $null }

        $fs.Position = $peOff
        if ($br.ReadUInt32() -ne 0x00004550) { return $null }    # 'PE\0\0'

        $machine   = $br.ReadUInt16()
        $null      = $br.ReadUInt16()                            # NumberOfSections
        $timeStamp = $br.ReadUInt32()

        switch ($machine) {
            0x014C  { $bits = 32; $arch = 'x86' }
            0x8664  { $bits = 64; $arch = 'x64' }
            0xAA64  { $bits = 64; $arch = 'ARM64' }
            0x01C4  { $bits = 32; $arch = 'ARM' }
            default { $bits = 0;  $arch = ('unknown machine 0x{0:X4}' -f $machine) }
        }

        $built = $null
        try { $built = ([datetime]'1970-01-01Z').ToUniversalTime().AddSeconds($timeStamp) } catch { }

        return New-Object psobject -Property @{
            Machine = $machine
            Bits    = $bits
            Arch    = $arch
            Built   = $built
        }
    }
    catch { return $null }
    finally {
        if ($br) { try { $br.Close() } catch { } }
        if ($fs) { try { $fs.Dispose() } catch { } }
    }
}

# RVA -> file offset, given a PE's section table. Split out from the reader below so the
# lookup does not depend on PowerShell's nested-function scoping.
function Convert-RvaToOffset
{
    param($Sections, [uint32] $Rva, [long] $Length)
    foreach ($s in $Sections) {
        if ($Rva -ge $s.V -and $Rva -lt ($s.V + $s.Span)) {
            $o = [long]$s.Raw + ([long]$Rva - [long]$s.V)
            if ($o -ge 0 -and $o -lt $Length) { return [long]$o }
            return [long](-1)
        }
    }
    return [long](-1)
}

# The names in a PE's export directory. $null when the file is not a readable PE; an empty
# array when it is one and exports nothing.
function Get-PeExportNames
{
    param([string] $Path, [int] $Max = 8192)
    if (-not (Test-FileHere $Path)) { return $null }

    $fs = $null
    $br = $null
    try {
        $fs = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        if ($fs.Length -lt 0x40) { return $null }
        $br = New-Object IO.BinaryReader($fs)

        if ($br.ReadUInt16() -ne 0x5A4D) { return $null }            # 'MZ'
        $fs.Position = 0x3C
        $peOff = $br.ReadInt32()
        if ($peOff -le 0 -or ($peOff + 24) -ge $fs.Length) { return $null }

        $fs.Position = $peOff
        if ($br.ReadUInt32() -ne 0x00004550) { return $null }        # 'PE\0\0'
        $null      = $br.ReadUInt16()                                # Machine
        $nSections = $br.ReadUInt16()
        $null      = $br.ReadUInt32()                                # TimeDateStamp
        $null      = $br.ReadUInt32()                                # PointerToSymbolTable
        $null      = $br.ReadUInt32()                                # NumberOfSymbols
        $optSize   = $br.ReadUInt16()
        $null      = $br.ReadUInt16()                                # Characteristics

        $optOff = [long]$peOff + 24
        if ($optSize -lt 96 -or ($optOff + $optSize) -gt $fs.Length) { return $null }
        $fs.Position = $optOff
        $magic = $br.ReadUInt16()
        # The data directories follow the optional header's fixed part: 96 bytes for PE32,
        # 112 for PE32+ (four fields widened to 64-bit).
        if     ($magic -eq 0x20B) { $dirOff = $optOff + 112 }
        elseif ($magic -eq 0x10B) { $dirOff = $optOff + 96 }
        else                      { return $null }
        if (($dirOff + 8) -gt $fs.Length) { return $null }
        $fs.Position = $dirOff
        $expRva = $br.ReadUInt32()                                   # DataDirectory[0] = exports
        $null   = $br.ReadUInt32()                                   # its size
        if ($expRva -eq 0) { return ,([string[]] @()) }

        $secOff = $optOff + $optSize
        $sections = @()
        for ($i = 0; $i -lt $nSections; $i++) {
            $p = $secOff + ([long]$i * 40)
            if (($p + 40) -gt $fs.Length) { break }
            $fs.Position = $p + 8                                    # past the 8-byte name
            $vsize = $br.ReadUInt32()
            $vaddr = $br.ReadUInt32()
            $rsize = $br.ReadUInt32()
            $raw   = $br.ReadUInt32()
            $span  = if ($vsize -gt 0) { $vsize } else { $rsize }
            $sections += New-Object psobject -Property @{ V = $vaddr; Span = $span; Raw = $raw }
        }
        if ($sections.Count -eq 0) { return $null }

        $eo = Convert-RvaToOffset $sections $expRva $fs.Length
        if ($eo -lt 0 -or ($eo + 40) -gt $fs.Length) { return $null }

        # IMAGE_EXPORT_DIRECTORY: NumberOfFunctions +20, NumberOfNames +24,
        # AddressOfFunctions +28, AddressOfNames +32.
        $fs.Position = $eo + 20
        $null     = $br.ReadUInt32()
        $nNames   = $br.ReadUInt32()
        $null     = $br.ReadUInt32()
        $namesRva = $br.ReadUInt32()
        if ($nNames -eq 0 -or $namesRva -eq 0) { return ,([string[]] @()) }
        if ($nNames -gt $Max) { $nNames = $Max }

        $no = Convert-RvaToOffset $sections $namesRva $fs.Length
        if ($no -lt 0) { return $null }

        $names = New-Object 'System.Collections.Generic.List[string]'
        for ($i = 0; $i -lt $nNames; $i++) {
            $p = $no + ([long]$i * 4)
            if (($p + 4) -gt $fs.Length) { break }
            $fs.Position = $p
            $so = Convert-RvaToOffset $sections ($br.ReadUInt32()) $fs.Length
            if ($so -lt 0) { continue }
            $fs.Position = $so
            $sb = New-Object Text.StringBuilder
            for ($k = 0; $k -lt 256; $k++) {
                $b = $fs.ReadByte()
                if ($b -le 0) { break }
                $null = $sb.Append([char]$b)
            }
            if ($sb.Length -gt 0) { $null = $names.Add($sb.ToString()) }
        }
        return ,([string[]] $names.ToArray())
    }
    catch { return $null }
    finally {
        if ($br) { try { $br.Close() } catch { } }
        if ($fs) { try { $fs.Dispose() } catch { } }
    }
}

# Does this ReShade build support add-ons?
#
# ReShade ships two builds. They carry the same version number and the same ProductName, so
# nothing this script checked could tell them apart -- and an install with the wrong one looks
# perfectly healthy while the add-on is never loaded at all (issue #53). It bites hardest on
# Vulkan, where the layer is machine-wide: anyone who has ever run ReShade's plain setup for
# any Vulkan game already has one, and it is reused for every game after that.
#
# The exports settle it, and not by inference: ReShade's own add-on API finds the ReShade
# module in a process by testing GetProcAddress for exactly "ReShadeRegisterAddon" and
# "ReShadeUnregisterAddon" (reshade.hpp). A build that does not export those two cannot load
# an add-on, because that is the mechanism by which add-ons find it.
#
# $null means the file could not be read -- never report that as a failure.
function Test-ReShadeHasAddons
{
    param([string] $Path)
    $names = Get-PeExportNames $Path
    if ($null -eq $names) { return $null }
    return [bool](($names -contains 'ReShadeRegisterAddon') -and ($names -contains 'ReShadeUnregisterAddon'))
}

# Scan a binary for an ASCII marker string. Deliberately capped: the NGX DLLs are 160 MB and
# there is never a reason to slurp one.
function Get-BinaryMarker
{
    param([string] $Path, [string] $Pattern, [int] $MaxBytes = 33554432)
    if (-not (Test-FileHere $Path)) { return $null }
    try {
        $fi = Get-Item -LiteralPath $Path -ErrorAction Stop
        if ($fi.Length -gt $MaxBytes -or $fi.Length -eq 0) { return $null }

        $fs = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try {
            $buf = New-Object byte[] $fi.Length
            $read = 0
            while ($read -lt $buf.Length) {
                $n = $fs.Read($buf, $read, $buf.Length - $read)
                if ($n -le 0) { break }
                $read += $n
            }
        }
        finally { $fs.Dispose() }

        $text = [Text.Encoding]::ASCII.GetString($buf, 0, $read)
        $m = [regex]::Match($text, $Pattern)
        if ($m.Success) {
            if ($m.Groups.Count -gt 1 -and $m.Groups[1].Success) { return $m.Groups[1].Value }
            return $m.Value
        }
    }
    catch { }
    return $null
}

function Read-TextSafe
{
    param([string] $Path, [int] $MaxBytes = 4194304)
    if (-not (Test-FileHere $Path)) { return $null }
    try {
        $fi = Get-Item -LiteralPath $Path -ErrorAction Stop
        if ($fi.Length -gt $MaxBytes) {
            # Only the tail of a huge log is interesting.
            $fs = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
            try {
                $fs.Position = $fi.Length - $MaxBytes
                $buf = New-Object byte[] $MaxBytes
                $n = $fs.Read($buf, 0, $MaxBytes)
                return (Remove-Bom ([Text.Encoding]::UTF8.GetString($buf, 0, $n)))
            }
            finally { $fs.Dispose() }
        }
        $fs = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try {
            $buf = New-Object byte[] $fi.Length
            $read = 0
            while ($read -lt $buf.Length) {
                $n = $fs.Read($buf, $read, $buf.Length - $read)
                if ($n -le 0) { break }
                $read += $n
            }
            return (Remove-Bom ([Text.Encoding]::UTF8.GetString($buf, 0, $read)))
        }
        finally { $fs.Dispose() }
    }
    catch { return $null }
}

# Files written by ReShade's installer start with a UTF-8 BOM. U+FEFF is not matched by \s in
# .NET regex, so a "^\s*Apps=" pattern silently fails on it -- strip it once, here.
function Remove-Bom
{
    param([string] $Text)
    if ($Text -and $Text.Length -gt 0 -and [int]$Text[0] -eq 0xFEFF) { return $Text.Substring(1) }
    return $Text
}

function Read-LinesSafe
{
    param([string] $Path)
    $t = Read-TextSafe $Path
    if ($null -eq $t) { return $null }
    return ($t -split "`r?`n")
}

# Value of one key in one section of an ini. Pass '' for $Section to read the ROOT
# (section-less) block at the top of the file, which is where ReShade keeps a preset's
# Techniques= and its preset-wide PreprocessorDefinitions=. Last occurrence wins, as
# ReShade's own parser does.
function Get-IniValue
{
    param([string] $Path, [string] $Section, [string] $Key)
    $lines = Read-LinesSafe $Path
    if ($null -eq $lines) { return $null }
    $cur   = ''
    $value = $null
    $rx    = '(?i)^' + [regex]::Escape($Key) + '\s*=\s*(.*)$'
    foreach ($ln in $lines) {
        $t = $ln.Trim()
        if ($t -match '^\[(.+)\]$') { $cur = $Matches[1]; continue }
        if (-not ($cur -ieq $Section)) { continue }
        if ($t -match $rx) { $value = $Matches[1] }
    }
    return $value
}

# One NAME=VALUE out of a ReShade PreprocessorDefinitions list (comma separated).
function Get-PreprocessorDefinition
{
    param([string] $Defs, [string] $Name)
    if (-not $Defs) { return $null }
    $m = [regex]::Match($Defs, '(?i)(?:^|[,;\s])' + [regex]::Escape($Name) + '\s*=\s*([^,;\s]+)')
    if ($m.Success) { return $m.Groups[1].Value }
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

# ---------------------------------------------------------------------------------------
# 0. Resolve the target folder and executable
# ---------------------------------------------------------------------------------------

Write-Banner

if (-not $GamePath -or -not $GamePath.Trim()) {
    $GamePath = (Get-Location).Path
}

$gameDir = $null
$exePath = $null

try {
    $resolved = (Resolve-Path -LiteralPath $GamePath -ErrorAction Stop).ProviderPath
}
catch {
    $resolved = $null
}

if (-not $resolved) {
    Report -Status 'Fail' -Text ('Path not found: ' + $GamePath) `
           -Action 'Pass a game folder, or the game''s .exe, that actually exists.'
    Write-Host ''
    Exit-Verifier 1
}

if (Test-DirHere $resolved) {
    $gameDir = $resolved
}
elseif (Test-FileHere $resolved) {
    $gameDir = [IO.Path]::GetDirectoryName($resolved)
    if ([IO.Path]::GetExtension($resolved) -match '(?i)^\.exe$') { $exePath = $resolved }
}

if (-not $gameDir) {
    Report -Status 'Fail' -Text ('Cannot make a game folder out of: ' + $GamePath) `
           -Action 'Point -GamePath at the folder holding the game''s real .exe.'
    Write-Host ''
    Exit-Verifier 1
}

if (-not $Quiet) {
    Write-Chunk ('  Folder  ') 'DarkGray' -NoNewline
    Write-Host $gameDir
}

# ---------------------------------------------------------------------------------------
# 1. Game architecture and render API
# ---------------------------------------------------------------------------------------

Write-Section 'Game executable and render API'

# -Exe wins. Otherwise pick the largest .exe that does not look like a launcher/helper.
$exeGuessed = $false
if ($Exe) {
    $cand = $Exe
    if (-not [IO.Path]::IsPathRooted($cand)) { $cand = Join-Safe $gameDir $Exe }
    if (Test-FileHere $cand) { $exePath = (Resolve-Path -LiteralPath $cand).ProviderPath }
    else {
        Report -Status 'Fail' -Text ('-Exe "' + $Exe + '" not found in the game folder.') `
               -Action 'Check the spelling, or drop -Exe and let the script pick.'
    }
}

if (-not $exePath) {
    $skip = '(?i)(launcher|unins|setup|crash|redist|vcredist|dxsetup|dxwebsetup|dgvoodoocpl|touchup|prereq|activation|helper|updater|report)'
    $exes = $null
    try {
        $exes = Get-ChildItem -LiteralPath $gameDir -File -Filter '*.exe' -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -notmatch $skip } |
                Sort-Object Length -Descending
    }
    catch { }

    if ($exes) {
        $exePath = $exes[0].FullName
        $exeGuessed = $true
    }
}

$gameBits = 0
$peGame = $null

if ($exePath) {
    $peGame = Get-PeInfo $exePath
    $exeName = [IO.Path]::GetFileName($exePath)
    $exeSize = ''
    try { $exeSize = ', ' + (Format-Size (Get-Item -LiteralPath $exePath).Length) } catch { }

    if ($null -eq $peGame) {
        Report -Status 'Warn' -Text ('Executable: ' + $exeName + ' -- PE header unreadable') `
               -Detail 'Cannot determine 32-bit vs 64-bit; assuming 64-bit for the checks below.'
        $gameBits = 64
    }
    elseif ($peGame.Bits -eq 0) {
        Report -Status 'Warn' -Text ('Executable: ' + $exeName + ' -- ' + $peGame.Arch) `
               -Detail 'Not an x86/x64 image; DLSS5-Feeder does not support this architecture.'
        $gameBits = 64
    }
    else {
        $gameBits = $peGame.Bits
        $note = ''
        if ($exeGuessed) { $note = "`nPicked automatically as the largest non-launcher .exe -- use -Exe to override." }
        Report -Status 'Ok' -Text ('Executable: ' + $exeName + ' (' + $peGame.Arch + ', ' + $gameBits + '-bit' + $exeSize + ')') `
               -Detail $note.TrimStart()
    }
}
else {
    Report -Status 'Warn' -Text 'No game executable found in this folder.' `
           -Detail 'Checks that depend on the game''s architecture will fall back to the local ReShade DLL, or to 64-bit.'
}

# Which local DLL, if any, is ReShade? A game's own d3d9.dll (dgVoodoo2's, say) is not.
function Test-IsReShade
{
    param([string] $Path)
    if (-not (Test-FileHere $Path)) { return $false }
    $n = Get-ProductNameSafe $Path
    if ($n -and $n -match '(?i)reshade') { return $true }
    # ReShade's own builds always carry the product name; if version info is missing entirely,
    # fall back to a marker scan rather than guessing from the file name.
    $m = Get-BinaryMarker -Path $Path -Pattern 'ReShade [0-9]+\.[0-9]+' -MaxBytes 16777216
    return [bool]$m
}

$reshadeLocal = $null
$reshadeLocalName = $null
foreach ($n in @('dxgi.dll', 'opengl32.dll', 'd3d11.dll', 'd3d9.dll', 'ddraw.dll', 'dinput8.dll')) {
    $p = Find-FileIn $gameDir $n
    if ($p -and (Test-IsReShade $p)) { $reshadeLocal = $p; $reshadeLocalName = [IO.Path]::GetFileName($p); break }
}

$dgVoodooConf = Find-FileIn $gameDir 'dgVoodoo.conf'
$d3d9Local    = Find-FileIn $gameDir 'd3d9.dll'

$api = 'unknown'
$apiDetail = ''
$isVulkan = $false

if ($d3d9Local -and $dgVoodooConf) {
    $api = 'Direct3D 9 via dgVoodoo2 (translated to D3D11)'
    $apiDetail = 'Inferred from a local d3d9.dll plus dgVoodoo.conf. ReShade hooks the D3D11 device dgVoodoo2 creates, so ReShade itself is the local dxgi.dll.'
}
elseif ($reshadeLocalName -eq 'opengl32.dll') {
    $api = 'OpenGL'
    $apiDetail = 'Inferred from a local ReShade opengl32.dll.'
}
elseif ($reshadeLocalName -eq 'dxgi.dll') {
    $api = 'Direct3D 10/11/12'
    $apiDetail = 'Inferred from a local ReShade dxgi.dll.'
}
elseif ($reshadeLocalName) {
    $api = 'Direct3D (hooked via ' + $reshadeLocalName + ')'
    $apiDetail = 'Inferred from the local ReShade DLL name.'
}
else {
    $api = 'Vulkan (machine-wide ReShade implicit layer)'
    $apiDetail = 'Guessed: there is no local ReShade DLL next to the exe, which is what the Vulkan install looks like. If the game is not a Vulkan game, ReShade is simply not installed here.'
    $isVulkan = $true
}

Report -Status 'Na' -Text ('Render API: ' + $api) -Detail $apiDetail

# ---------------------------------------------------------------------------------------
# 2. ReShade
# ---------------------------------------------------------------------------------------

Write-Section 'ReShade'

function Test-ReShadeVersion
{
    param([string] $Version)
    # Returns $true when >= 6.8 (RESHADE_API_VERSION 20, which this add-on needs).
    if (-not $Version) { return $null }
    $m = [regex]::Match($Version, '^\s*(\d+)\.(\d+)')
    if (-not $m.Success) { return $null }
    $maj = [int]$m.Groups[1].Value
    $min = [int]$m.Groups[2].Value
    if ($maj -gt 6) { return $true }
    if ($maj -eq 6 -and $min -ge 8) { return $true }
    return $false
}

function Report-ReShadeDll
{
    param([string] $Path, [string] $Label)

    $ver = Get-FileVersionSafe $Path
    if (-not $ver) {
        Report -Status 'Warn' -Text ($Label + ': version info unreadable') `
               -Detail $Path
        return
    }
    # Add-on support first: a new-enough ReShade WITHOUT it is the more confusing failure,
    # because every other check passes and nothing in any log says the add-on was refused --
    # it is simply never looked for (issue #53).
    $addons = Test-ReShadeHasAddons $Path
    if ($addons -eq $false) {
        Report -Status 'Fail' -Text ($Label + ': ' + $ver + ' -- built WITHOUT add-on support') `
               -Detail ('This is ReShade''s plain build. It does not export ReShadeRegisterAddon, which is how an add-on finds ReShade, so dlss5-feed is never loaded and nothing anywhere says so. The version number and ProductName are identical to the add-on build, which is why this is easy to miss. On Vulkan the layer is machine-wide, so one plain install from any earlier game is reused for every game after it.') `
               -Action ('Re-run the ReShade installer over ' + $Path + ' and tick "Enable loading of add-ons" (the ReShade_Setup_*_Addon.exe download).')
        return
    }

    $ok = Test-ReShadeVersion $ver
    if ($ok -eq $true) {
        $d = $Path
        if ($null -eq $addons) { $d = $Path + "`nAdd-on support could not be read from this file; assuming it is present." }
        Report -Status 'Ok' -Text ($Label + ': ' + $ver + $(if ($addons -eq $true) { ' (add-on build)' } else { '' })) -Detail $d
    }
    elseif ($ok -eq $false) {
        Report -Status 'Fail' -Text ($Label + ': ' + $ver + ' -- too old') `
               -Detail 'The add-on needs ReShade API version 20, i.e. ReShade 6.8 or newer. A 6.7.x install loads silently and then refuses the add-on.' `
               -Action ('Reinstall ReShade 6.8+ (add-on support enabled) over ' + $Path)
    }
    else {
        Report -Status 'Warn' -Text ($Label + ': unrecognised version string "' + $ver + '"') -Detail $Path
    }
}

if ($reshadeLocal) {
    Report-ReShadeDll -Path $reshadeLocal -Label ('Local ReShade (' + $reshadeLocalName + ')')

    if ($gameBits -gt 0 -and $peGame) {
        $peRs = Get-PeInfo $reshadeLocal
        if ($peRs -and $peRs.Bits -gt 0 -and $peRs.Bits -ne $gameBits) {
            Report -Status 'Fail' -Text ('ReShade DLL is ' + $peRs.Bits + '-bit but the game is ' + $gameBits + '-bit') `
                   -Action ('Replace ' + $reshadeLocalName + ' with the ' + $gameBits + '-bit ReShade build.')
        }
    }
}
else {
    Report -Status 'Na' -Text 'No local ReShade DLL next to the exe (expected for the Vulkan install).'
}

$pdRoot = 'C:\ProgramData\ReShade'
$needVulkanCheck = ($isVulkan -or -not $reshadeLocal)

if ($needVulkanCheck) {
    if ($gameBits -eq 32) { $pdDllName = 'ReShade32.dll' } else { $pdDllName = 'ReShade64.dll' }
    $pdDll = Find-FileIn $pdRoot $pdDllName

    if ($pdDll) {
        Report-ReShadeDll -Path $pdDll -Label ('Vulkan layer (' + $pdDllName + ')')
    }
    else {
        Report -Status 'Fail' -Text ('Machine-wide ' + $pdDllName + ' not found under ' + $pdRoot) `
               -Detail 'Without it, ReShade never loads into a Vulkan game (and there is no local DLL here either).' `
               -Action 'Run the ReShade installer, pick the game''s exe, choose Vulkan, and enable add-on support.'
    }

    $appsIni = Find-FileIn $pdRoot 'ReShadeApps.ini'
    if (-not $appsIni) {
        Report -Status 'Warn' -Text 'ReShadeApps.ini not found -- cannot confirm this game is allowed to load ReShade.' `
               -Detail ($pdRoot + '\ReShadeApps.ini')
    }
    else {
        $appsText = Read-TextSafe $appsIni
        if ($null -eq $appsText) {
            Report -Status 'Warn' -Text 'ReShadeApps.ini is unreadable (permissions?).' -Detail $appsIni
        }
        elseif (-not $exePath) {
            Report -Status 'Warn' -Text 'No game exe identified, so the ReShadeApps.ini gate cannot be checked.'
        }
        else {
            $m = [regex]::Match($appsText, '(?im)^\s*Apps\s*=\s*(.*)$')
            $entries = @()
            if ($m.Success) {
                $entries = $m.Groups[1].Value -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }
            }
            $listed = $false
            foreach ($e in $entries) {
                if ($e -and ($e.TrimEnd('\') -ieq $exePath.TrimEnd('\'))) { $listed = $true; break }
            }
            if ($listed) {
                Report -Status 'Ok' -Text 'Game exe is on the ReShadeApps.ini Apps= list.' `
                       -Detail ($entries.Count.ToString() + ' entries in ' + $appsIni)
            }
            else {
                $stale = @()
                $leaf = [IO.Path]::GetFileName($exePath)
                foreach ($e in $entries) { if ([IO.Path]::GetFileName($e) -ieq $leaf) { $stale += $e } }
                $d = 'ReShade only activates for exes on that comma-separated list.'
                if ($stale.Count -gt 0) {
                    $d += "`nA stale entry for the same exe name is present (the game probably moved drives): " + ($stale -join '; ')
                }
                $d += "`nThat file is not writable without elevation, so this script only reports it."
                $q = [char]0x27
                $fix = 'From an ELEVATED PowerShell, back up ' + $appsIni +
                       ' and append this exe to its Apps= line:' + "`n" +
                       '$p = ' + $q + $appsIni + $q + '; Copy-Item $p "$p.bak"; ' +
                       '$c = (Get-Content -Raw $p).TrimEnd() + ' + $q + ',' + $exePath + $q + '; ' +
                       '[IO.File]::WriteAllText($p, $c, (New-Object Text.UTF8Encoding $true))'
                Report -Status 'Fail' -Text 'Game exe is NOT on the ReShadeApps.ini Apps= list.' -Detail $d `
                       -Action $fix
            }
        }
    }
}
else {
    Report -Status 'Na' -Text 'Vulkan implicit-layer checks skipped (this is a local-DLL install).'
}

$reshadeIni = Find-FileIn $gameDir 'ReShade.ini'
if ($reshadeIni) { Report -Status 'Ok' -Text 'ReShade.ini present.' }
else { Report -Status 'Warn' -Text 'No ReShade.ini in the game folder -- ReShade will create defaults, which may not find the shaders.' }

# ---------------------------------------------------------------------------------------
# 3. The feeder's own files
# ---------------------------------------------------------------------------------------

Write-Section 'DLSS5-Feeder'

if ($gameBits -eq 32) { $addonName = 'dlss5-feed.addon32'; $wrongAddon = 'dlss5-feed.addon64' }
else                  { $addonName = 'dlss5-feed.addon64'; $wrongAddon = 'dlss5-feed.addon32' }

$addonPath = Find-FileIn $gameDir $addonName
$feedVersion = $null

if ($addonPath) {
    $feedVersion = Get-BinaryMarker -Path $addonPath -Pattern 'DLSS 5 Feed (?:\(32-bit\) )?(\d[\w.\-+]*)'
    $peAddon = Get-PeInfo $addonPath
    $d = ''
    if ($peAddon -and $peAddon.Bits -gt 0) { $d = $peAddon.Arch + ' image' }
    if ($feedVersion) {
        Report -Status 'Ok' -Text ($addonName + ': version ' + $feedVersion) -Detail $d
    }
    else {
        Report -Status 'Warn' -Text ($addonName + ': present, but no version string found inside it.') `
               -Detail ('Expected an exported NAME of "DLSS 5 Feed <version>". ' + $d).Trim()
    }
    if ($peAddon -and $peAddon.Bits -gt 0 -and $gameBits -gt 0 -and $peAddon.Bits -ne $gameBits) {
        Report -Status 'Fail' -Text ($addonName + ' is a ' + $peAddon.Bits + '-bit image but the game is ' + $gameBits + '-bit.') `
               -Action ('Deploy the ' + $gameBits + '-bit add-on build.')
    }
}
else {
    Report -Status 'Fail' -Text ($addonName + ' is missing.') `
           -Detail ('This is the feeder itself; without it nothing else here does anything. Expected next to ' + $gameDir) `
           -Action ('Copy ' + $addonName + ' into the game folder.')
}

$strayAddon = Find-FileIn $gameDir $wrongAddon
if ($strayAddon) {
    Report -Status 'Warn' -Text ($wrongAddon + ' is also present, and is for the other architecture.') `
           -Detail 'Harmless (ReShade will not load it) but it makes the install confusing -- remove it.'
}

# The 32-bit split-process path needs the 64-bit host helper.
$hostDir = Join-Safe $gameDir 'host64'
$hostExe = $null

if ($gameBits -eq 32) {
    $hostExe = Find-FileIn $hostDir 'dlss5-feed-host64.exe'
    if ($hostExe) {
        $peHost = Get-PeInfo $hostExe
        $d = ''
        if ($peHost -and $peHost.Built) { $d = 'linked ' + $peHost.Built.ToString('yyyy-MM-dd HH:mm') + ' UTC' }
        Report -Status 'Ok' -Text 'host64\dlss5-feed-host64.exe present.' -Detail $d

        # The host exe carries no version string, so compare PE link timestamps: the two halves
        # are built together, and a gap means they came from different releases. The host also
        # reports a real protocol mismatch in its log, which the log section below picks up.
        if ($addonPath) {
            $peAddon2 = Get-PeInfo $addonPath
            if ($peAddon2 -and $peHost -and $peAddon2.Built -and $peHost.Built) {
                $gap = [math]::Abs(($peAddon2.Built - $peHost.Built).TotalHours)
                if ($gap -gt 24) {
                    Report -Status 'Warn' -Text 'The add-on and the host64 helper look like different builds.' `
                           -Detail ('dlss5-feed.addon32 linked ' + $peAddon2.Built.ToString('yyyy-MM-dd HH:mm') +
                                    ' UTC, host64 linked ' + $peHost.Built.ToString('yyyy-MM-dd HH:mm') + ' UTC (' +
                                    [math]::Round($gap / 24, 1) + ' days apart).' +
                                    "`nA version mismatch across the IPC pipe is a real failure mode -- the host refuses the connection and neural rendering never starts. Redeploy both halves from the same release.")
                }
                else {
                    Report -Status 'Ok' -Text 'Add-on and host64 helper are from the same build (link timestamps agree).'
                }
            }
        }
    }
    else {
        Report -Status 'Fail' -Text 'host64\dlss5-feed-host64.exe is missing.' `
               -Detail 'A 32-bit game needs the 64-bit helper process -- the x86 add-on cannot talk to NGX itself.' `
               -Action 'Create host64\ next to the game exe and copy dlss5-feed-host64.exe into it.'
    }
}
else {
    Report -Status 'Na' -Text 'host64\ helper not applicable (64-bit game runs in-process).'
}

# Shader + framework headers.
$shaderDir = Join-Safe $gameDir 'reshade-shaders\Shaders'
$fx = Find-FileUnder $shaderDir 'DLSS5_Feed.fx'
if ($fx) {
    $d = ''
    $parent = [IO.Path]::GetDirectoryName($fx)
    if ($parent -and -not ($parent -ieq $shaderDir)) { $d = 'in a sub-folder: ' + $parent }
    Report -Status 'Ok' -Text 'reshade-shaders\Shaders\DLSS5_Feed.fx present.' -Detail $d
}
else {
    Report -Status 'Fail' -Text 'reshade-shaders\Shaders\DLSS5_Feed.fx is missing.' `
           -Detail 'This is the effect that hands the add-on colour, depth and motion vectors.' `
           -Action 'Copy DLSS5_Feed.fx into reshade-shaders\Shaders\.'
}

# Headers may legitimately sit in a sub-folder (Shaders\CrosireMaster\, say): ReShade's
# default EffectSearchPaths ends in "**", so the compiler finds them there.
$missingHeaders = @()
foreach ($h in @('ReShade.fxh', 'ReShadeUI.fxh', 'DrawText.fxh')) {
    if (-not (Find-FileUnder $shaderDir $h)) { $missingHeaders += $h }
}
if ($missingHeaders.Count -eq 0) {
    Report -Status 'Ok' -Text 'ReShade framework headers present (ReShade.fxh, ReShadeUI.fxh, DrawText.fxh).'
}
else {
    Report -Status 'Fail' -Text ('Missing framework header(s): ' + ($missingHeaders -join ', ')) `
           -Detail 'Every .fx includes these; without them DLSS5_Feed.fx fails to compile and the add-on is fed nothing.' `
           -Action 'Copy the missing .fxh files from crosire/reshade-shaders into reshade-shaders\Shaders\.'
}

# ---------------------------------------------------------------------------------------
# 4. The neural consumer
# ---------------------------------------------------------------------------------------

Write-Section 'Neural consumer'

if ($gameBits -eq 32) {
    $consumerDir = $hostDir
    $consumerWhere = 'host64\'
}
else {
    $consumerDir = $gameDir
    $consumerWhere = 'the game folder'
}

$dfcAddon   = Find-FileIn $consumerDir 'deep-fried-chicken.addon64'
$renoAddon  = Find-FileIn $consumerDir 'renodx-dlss5.addon64'
$toolkit    = Find-FileIn $consumerDir 'alexs-toolkit.addon64'
$dx11Bridge = Find-FileIn $consumerDir 'dlss5-dx11-bridge.addon64'

if ($gameBits -eq 32) {
    # A 64-bit add-on beside a 32-bit exe is the single most common 32-bit deploy mistake.
    foreach ($n in @('deep-fried-chicken.addon64', 'renodx-dlss5.addon64', 'alexs-toolkit.addon64')) {
        $stray = Find-FileIn $gameDir $n
        if ($stray) {
            Report -Status 'Fail' -Text ($n + ' is next to the 32-bit game exe -- wrong place.') `
                   -Detail 'This game is 32-bit, so the neural consumer must live in host64\ where the 64-bit helper process loads it. A 64-bit add-on beside an x86 exe is never loaded by anything.' `
                   -Action ('Move ' + $n + ' into ' + $hostDir)
        }
    }
    # And the mirror image of it: the feeder's own 64-bit add-on inside host64\. Unlike a
    # stray consumer beside the exe this one does load -- host64\ is a 64-bit ReShade
    # install, so the helper picks it up and runs the add-on meant for a 64-bit GAME inside
    # the very process that is serving the 32-bit one. It opens a second NGX session on its
    # own private device and detours nvngx over the consumer's hooks, in a process where
    # none of that is tested. Recent builds recognise the helper and stay inert, but an
    # older one does not, and either way the file should not be there.
    $strayFeeder = Find-FileIn $hostDir 'dlss5-feed.addon64'
    if ($strayFeeder) {
        Report -Status 'Fail' -Text 'host64\dlss5-feed.addon64 should not be there.' `
               -Detail 'That is the add-on for a 64-bit GAME. The helper''s own ReShade loads every add-on in host64\, so this one ends up inside the helper process, where it can do nothing useful and gets in the way of the neural consumer. host64\ takes dlss5-feed-host64.exe, a 64-bit ReShade dxgi.dll, the neural consumer and the nvngx runtimes -- nothing else.' `
               -Action ('Delete ' + $strayFeeder)
    }
    if (-not (Test-DirHere $hostDir)) {
        Report -Status 'Fail' -Text 'host64\ does not exist, so there is nowhere for the neural consumer to live.' `
               -Action 'Create host64\ and deploy the helper, the neural consumer and the NVIDIA DLLs into it.'
    }
}

if ($dfcAddon -and $renoAddon) {
    Report -Status 'Fail' -Text 'BOTH Deep Fried Chicken and the RenoDX DLSS 5 add-on are present.' `
           -Detail 'Deep Fried Chicken goes completely inert for the whole process while a RenoDX neural provider is loaded. Everything still looks healthy -- frames are delivered, no errors -- and neural rendering does nothing.' `
           -Action ('Remove one of them from ' + $consumerDir + ' (keep deep-fried-chicken.addon64 unless you specifically want RenoDX).')
}
elseif ($dfcAddon) {
    $dfcVer = Get-BinaryMarker -Path $dfcAddon -Pattern 'Deep Fried Chicken (\d[\w.\-+]*)'
    if ($dfcVer) { $t = 'Deep Fried Chicken ' + $dfcVer + ' (recommended default).' }
    else         { $t = 'Deep Fried Chicken present (version string not found).' }
    Report -Status 'Ok' -Text $t -Detail ('in ' + $consumerWhere)

    foreach ($f in @('deep-fried-chicken-nvngx.dll', 'deep-fried-chicken.cfg')) {
        if (Find-FileIn $consumerDir $f) {
            Report -Status 'Ok' -Text ($f + ' present.')
        }
        elseif ($f -match 'nvngx') {
            Report -Status 'Fail' -Text ($f + ' is missing.') `
                   -Detail 'This is Chicken''s private NGX bridge (a few KB, not a copy of NVIDIA''s). Without it Chicken cannot attach.' `
                   -Action ('Copy ' + $f + ' into ' + $consumerDir)
        }
        else {
            Report -Status 'Warn' -Text ($f + ' is missing -- Chicken will fall back to its built-in defaults.')
        }
    }
}
elseif ($renoAddon) {
    Report -Status 'Ok' -Text 'renodx-dlss5.addon64 present (supported alternative).' `
           -Detail ('in ' + $consumerWhere + '. Deep Fried Chicken is the recommended default.')
}
else {
    Report -Status 'Fail' -Text 'No neural consumer found.' `
           -Detail ('Expected deep-fried-chicken.addon64 (recommended) or renodx-dlss5.addon64 in ' + $consumerDir + '. The feeder publishes a synthetic DLSS contract; without a consumer, nothing acts on it.') `
           -Action ('Copy deep-fried-chicken.addon64 (+ deep-fried-chicken-nvngx.dll and deep-fried-chicken.cfg) into ' + $consumerDir)
}

if ($toolkit) {
    if ($dfcAddon) {
        Report -Status 'Warn' -Text 'alexs-toolkit.addon64 is present alongside Deep Fried Chicken.' `
               -Detail 'That is a third interposer on the same NGX module. Chicken''s own test notes ask for the toolkit to be removed -- do not combine them.'
    }
    else {
        Report -Status 'Warn' -Text 'alexs-toolkit.addon64 is present (optional multi-pass cascade).' `
               -Detail 'Fine with the RenoDX provider; check alexs-toolkit.log for "cascade interception is now armed" to confirm it won the load-order race.'
    }
}
else {
    Report -Status 'Na' -Text 'Alex''s Toolkit not installed (optional; off the default path).'
}

if ($dx11Bridge) {
    Report -Status 'Fail' -Text 'dlss5-dx11-bridge.addon64 is present.' `
           -Detail 'This must never be combined with DLSS5-Feeder -- the feeder already provides the contract, and the bridge fights it.' `
           -Action ('Delete dlss5-dx11-bridge.addon64 from ' + $consumerDir)
}

# ---------------------------------------------------------------------------------------
# 5. NVIDIA runtimes
# ---------------------------------------------------------------------------------------

Write-Section 'NVIDIA NGX runtimes'

foreach ($n in @('nvngx_dlssnr.dll', 'nvngx_dlss.dll')) {
    $p = Find-FileIn $consumerDir $n
    if ($p) {
        $v = Get-FileVersionSafe $p
        if ($v) { $t = $n + ': ' + $v } else { $t = $n + ': present (no version info)' }
        # NVIDIA's own build and ShortFuse's .SF repack both carry file version 310.8.0.0, so
        # the version alone cannot tell them apart -- and that is exactly the distinction
        # issue #47 turns on. The product/description strings do differ, so print them.
        $d = ('in ' + $consumerWhere)
        $pn = Get-ProductNameSafe $p
        if ($pn) { $d = $d + "`n" + $pn }
        # Two more fields, and neither is asserted to be decisive. This used to call
        # OriginalFilename ("CL 38718415") the sharpest tell there is; #50's reporter says the
        # changelist is identical on both builds and what differs is the stated FileVersion
        # STRING -- "310,8,0,0" from NVIDIA, "310.8.SF.0" from the repack. That string is a
        # separate field from the version quad above and was never being read. Print both and
        # let whoever compares two machines decide.
        try {
            $vi = (Get-Item -LiteralPath $p -ErrorAction Stop).VersionInfo
            if ($vi.OriginalFilename) { $d = $d + "`nbuild: " + $vi.OriginalFilename.Trim() }
            # Only when it disagrees with the quad: on a stock runtime it is the same numbers
            # with commas, and repeating it would train people to skip the line.
            if ($vi.FileVersion) {
                $stated = ($vi.FileVersion -replace '\s', '') -replace ',', '.'
                if ($stated -and $stated -ne $v) { $d = $d + "`nstated FileVersion: " + $vi.FileVersion.Trim() }
            }
        }
        catch { }
        Report -Status 'Ok' -Text $t -Detail $d
    }
    else {
        if ($n -eq 'nvngx_dlssnr.dll') { $why = 'This is the neural-rendering model itself -- DLSS 5 cannot run without it.' }
        else { $why = 'The DLSS super-resolution runtime; the neural consumer expects it beside the NR model.' }
        Report -Status 'Fail' -Text ($n + ' is missing from ' + $consumerWhere + '.') -Detail $why `
               -Action ('Copy ' + $n + ' into ' + $consumerDir)
    }
}

# ---------------------------------------------------------------------------------------
# 6. The d3dcompiler_47.dll trap
# ---------------------------------------------------------------------------------------

Write-Section 'd3dcompiler_47.dll'

$dcDirs = @(@{ Dir = $gameDir; Label = 'game folder' })
if ($gameBits -eq 32 -and (Test-DirHere $hostDir)) { $dcDirs += @{ Dir = $hostDir; Label = 'host64\' } }

$foundAny = $false
foreach ($e in $dcDirs) {
    $p = Find-FileIn $e.Dir 'd3dcompiler_47.dll'
    if (-not $p) { continue }
    $foundAny = $true
    $v = Get-FileVersionSafe $p
    if (-not $v) {
        Report -Status 'Warn' -Text ('A d3dcompiler_47.dll sits in the ' + $e.Label + ' but its version is unreadable.') `
               -Detail ($p + "`nWindows loads this copy in preference to System32's. If it predates Shader Model 5.1 the neural pass silently fails every frame.")
    }
    elseif ($v -match '^6\.3\.') {
        Report -Status 'Fail' -Text ('Windows 8.1-era d3dcompiler_47.dll in the ' + $e.Label + ': ' + $v) `
               -Detail ($p + "`nThis copy wins the DLL search order over System32's and knows nothing past Shader Model 5.0. The neural pass compiles as cs_5_1, so it fails EVERY FRAME, silently: the log still reports frames delivered and neural rendering does nothing. ReShade.log shows: error X3506: unrecognized compiler target 'cs_5_1'.") `
               -Action ('Delete or rename ' + $p + ' (System32''s copy is then used, which is fine).')
    }
    else {
        Report -Status 'Ok' -Text ('d3dcompiler_47.dll in the ' + $e.Label + ': ' + $v + ' -- new enough for cs_5_1.') -Detail $p
    }
}
if (-not $foundAny) {
    Report -Status 'Ok' -Text 'No local d3dcompiler_47.dll -- System32''s copy is used, which is correct.'
}

# ---------------------------------------------------------------------------------------
# 7. GPU
# ---------------------------------------------------------------------------------------

Write-Section 'GPU'

$gpus = $null
try { $gpus = Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop }
catch {
    try { $gpus = Get-WmiObject -Class Win32_VideoController -ErrorAction Stop } catch { $gpus = $null }
}

if (-not $gpus) {
    Report -Status 'Warn' -Text 'Could not query the display adapters (WMI/CIM unavailable).' `
           -Detail 'DLSS needs an NVIDIA RTX card; verify manually.'
}
else {
    $rtx = @($gpus | Where-Object { $_.Name -and $_.Name -match '(?i)nvidia' -and $_.Name -match '(?i)(RTX|TITAN RTX)' })
    $names = @($gpus | ForEach-Object { $_.Name }) -join '; '
    if ($rtx.Count -gt 0) {
        Report -Status 'Ok' -Text ('NVIDIA RTX adapter found: ' + (($rtx | ForEach-Object { $_.Name }) -join '; '))

        # The driver version. Win32_VideoController already carries it, so this costs no
        # extra query -- and without it "the driver is too old" and issue #47's NGX
        # 0xBAD00001 are indistinguishable from this script's output. One reporter passed
        # with 14 OK / 0 failures on 596.36, a configuration that cannot work at all.
        # NVIDIA's branding is the last five digits of the Windows version, dotted.
        $drv = @($rtx | ForEach-Object { $_.DriverVersion } | Where-Object { $_ })
        if ($drv.Count -eq 0) {
            Report -Status 'Warn' -Text 'The NVIDIA driver version could not be read.' `
                   -Detail ('DLSS 5 neural rendering needs ' + $kMinDriver + ' or newer; check it in the NVIDIA app.')
        }
        else {
            # Compare the five digits as an INTEGER, never as a decimal: [double] '616.56'
            # goes through the current culture in PowerShell 5.1, and on a comma-decimal
            # machine that silently reads as 61656 while the literal reads as 616.56.
            $digits = ($drv[0] -replace '[^0-9]', '')
            $shown  = $drv[0]
            $num    = $null
            if ($digits.Length -ge 5) {
                $tail  = $digits.Substring($digits.Length - 5)
                $shown = $tail.Substring(0, 3) + '.' + $tail.Substring(3)
                $num   = [int]$tail
            }
            if ($null -eq $num) {
                Report -Status 'Na' -Text ('NVIDIA driver: ' + $shown) `
                       -Detail ('Could not compare against the ' + $kMinDriver + ' minimum; check it by hand.')
            }
            elseif ($num -lt $kMinDriverDigits) {
                Report -Status 'Fail' -Text ('NVIDIA driver ' + $shown + ' is older than ' + $kMinDriver + '.') `
                       -Detail 'DLSS 5 neural rendering ships in the driver, and NGX reports FeatureNotSupported (0xBAD00001) on older ones -- which looks exactly like a bug in this project.' `
                       -Action ('Update the NVIDIA driver to ' + $kMinDriver + ' or newer, then re-run this script.')
            }
            else {
                Report -Status 'Ok' -Text ('NVIDIA driver: ' + $shown) -Detail ('Minimum for neural rendering is ' + $kMinDriver + '.')
            }
        }

        if (@($gpus).Count -gt 1) {
            Report -Status 'Na' -Text ('Other adapters present: ' + $names) `
                   -Detail 'On a hybrid machine, confirm the game actually renders on the RTX GPU -- the interop extensions do not exist on an iGPU.'
        }
    }
    else {
        Report -Status 'Warn' -Text 'No NVIDIA RTX adapter detected.' `
               -Detail ('Adapters: ' + $names + "`nDLSS/NGX requires an RTX card; nothing below will work without one.")
    }
}

# ---------------------------------------------------------------------------------------
# 8. Logs
# ---------------------------------------------------------------------------------------

Write-Section 'Logs'

function Report-FeedLog
{
    # $Half is '' for the ordinary in-process case, or 'game'/'host' for the 32-bit split
    # path, where the two halves each only ever log one side of the story.
    param([string] $Path, [string] $Label, [string] $Half = '')

    $lines = Read-LinesSafe $Path
    if ($null -eq $lines) {
        Report -Status 'Warn' -Text ($Label + ' exists but could not be read.') -Detail $Path
        return
    }

    # Strip the "HH:MM:SS.mmm  [feed] " / "[host] " prefix so the digest reads as prose.
    function Strip-Prefix { param([string] $S) return (($S -replace '^\s*[\d:.]+\s+', '') -replace '^\[(feed|host)\]\s*', '').Trim() }

    $verLine = @($lines | Where-Object { $_ -match '(?i)dlss5-feed\S*\s.*\(built' }) | Select-Object -Last 1
    if (-not $verLine) { $verLine = @($lines | Where-Object { $_ -match '(?i)dlss5-feed\S*\s.*attached\.' }) | Select-Object -Last 1 }
    if ($verLine) { Report -Status 'Na' -Text ($Label + ': ' + (Strip-Prefix $verLine)) }
    else { Report -Status 'Na' -Text ($Label + ': present (' + $lines.Count + ' lines), no version banner found.') }

    $ready = @($lines | Where-Object { $_ -match '(?i)feature ready' }) | Select-Object -Last 1
    if ($ready) {
        Report -Status 'Ok' -Text (Strip-Prefix $ready)
    }
    elseif ($Half -eq 'game') {
        Report -Status 'Na' -Text ($Label + ': no "feature ready" line -- expected here; the host64 helper creates the feature.')
    }
    else {
        Report -Status 'Warn' -Text ($Label + ': no "feature ready" line -- the DLSS feature was never created in the last run.')
    }

    $delivered = @($lines | Where-Object { $_ -match '(?i)frame \d+ delivered' }) | Select-Object -Last 1
    if ($delivered) {
        $m = [regex]::Match($delivered, 'frame (\d+) delivered')
        if ($m.Success) { Report -Status 'Ok' -Text ('frames delivered: last seen frame ' + $m.Groups[1].Value + '.') }
        else { Report -Status 'Ok' -Text 'frames delivered.' }
    }
    elseif ($Half -eq 'host') {
        Report -Status 'Na' -Text ($Label + ': no "frame N delivered" line -- expected here; the game-side add-on counts delivered frames.')
    }
    else {
        Report -Status 'Warn' -Text ($Label + ': no "frame N delivered" line -- nothing was ever fed to the consumer.')
    }

    # The "[feed] effects:" line carries the provider the effect was actually COMPILED with,
    # which is what section 9 should believe over any of the three ini levels (issue #50).
    $effects = @($lines | Where-Object { $_ -match '(?i)DLSS5_MV_PROVIDER\s*=\s*\d' }) | Select-Object -Last 1
    if ($effects) {
        $m = [regex]::Match($effects, '(?i)DLSS5_MV_PROVIDER\s*=\s*(\d+)')
        if ($m.Success -and -not $script:FeedLogProvider) { $script:FeedLogProvider = $m.Groups[1].Value }
    }

    $bad = @($lines | Where-Object { $_ -match '(?i)(WARNING|not loaded|disabl|TOO OLD|different releases|refus)' } |
                      Select-Object -Unique)

    # "DLSS5_Feed.fx is not loaded" is logged once at attach, before ReShade has compiled any
    # effect. If that same run went on to deliver frames, it was only the start-up transient.
    if ($delivered) { $bad = @($bad | Where-Object { $_ -notmatch '(?i)DLSS5_Feed\.fx is not loaded' }) }
    $bad = @($bad | Select-Object -Last 5)

    if ($bad.Count -gt 0) {
        foreach ($b in $bad) {
            $txt = Strip-Prefix $b
            if ($txt.Length -gt 160) { $txt = $txt.Substring(0, 157) + '...' }
            if ($b -match '(?i)(different releases|TOO OLD)') {
                Report -Status 'Fail' -Text ($Label + ': ' + $txt) `
                       -Action 'Redeploy both halves from the same release / fix the flagged file, then re-run the game once.'
            }
            else {
                Report -Status 'Warn' -Text ($Label + ': ' + $txt)
            }
        }
    }
    else {
        Report -Status 'Ok' -Text ($Label + ': no warnings or disable reasons in the log.')
    }
}

$anyLog = $false

if ($gameBits -eq 32) { $gameHalf = 'game' } else { $gameHalf = '' }

$feedLog = Find-FileIn $gameDir 'dlss5-feed.log'
if ($feedLog) { $anyLog = $true; Report-FeedLog -Path $feedLog -Label 'dlss5-feed.log' -Half $gameHalf }

if ($gameBits -eq 32 -and (Test-DirHere $hostDir)) {
    $hostLog = Find-FileIn $hostDir 'dlss5-feed-host.log'
    if ($hostLog) { $anyLog = $true; Report-FeedLog -Path $hostLog -Label 'host64\dlss5-feed-host.log' -Half 'host' }
}

if (-not $anyLog) {
    Report -Status 'Na' -Text 'No feeder log yet -- launch the game once and re-run this script for a runtime verdict.'
}

$dfcLog = Find-FileIn $consumerDir 'deep-fried-chicken.log'
if ($dfcLog) {
    $lines = Read-LinesSafe $dfcLog
    if ($null -eq $lines) {
        Report -Status 'Warn' -Text 'deep-fried-chicken.log exists but could not be read.' -Detail $dfcLog
    }
    else {
        $marker = @($lines | Where-Object { $_ -match 'feeder_marker=' }) | Select-Object -Last 1
        if ($marker -and $marker -match 'feeder_marker=1') {
            $extra = ''
            if ($marker -match 'legacy_exact=(\d)') { $extra = 'legacy_exact=' + $Matches[1] }
            Report -Status 'Ok' -Text 'Chicken accepted the interop marker (feeder_marker=1).' -Detail $extra
        }
        elseif ($marker) {
            Report -Status 'Fail' -Text 'Chicken did NOT accept the interop marker (feeder_marker=0).' `
                   -Detail ($marker.Trim() + "`nChicken either never saw our DFC.Feeder.* parameters, or fell back to recognising us by file name.") `
                   -Action 'Make sure the add-on and Chicken are both recent enough to speak interop ABI 1 (feeder 0.10.0+, Chicken 1.4.0-alpha+).'
        }
        else {
            Report -Status 'Warn' -Text 'deep-fried-chicken.log has no feeder_marker= line yet.' `
                   -Detail 'That line only appears once a native DLSS create goes through. Run the game long enough to reach gameplay and re-check.'
        }

        $state = @($lines | Where-Object { $_ -match 'interop_state=' }) | Select-Object -Last 1
        if ($state -and $state -match 'interop_state=(\w+)') {
            $s = $Matches[1]
            if ($s -match '(?i)^(ARMED|CLAIMING)$') { Report -Status 'Ok' -Text ('Chicken interop_state=' + $s + '.') }
            else { Report -Status 'Warn' -Text ('Chicken interop_state=' + $s + ' -- not armed.') }
        }
        else {
            Report -Status 'Warn' -Text 'deep-fried-chicken.log has no interop_state= line.'
        }
    }
}
elseif ($dfcAddon) {
    Report -Status 'Na' -Text 'No deep-fried-chicken.log yet -- Chicken has not run here.'
}

# ---------------------------------------------------------------------------------------
# 9. Motion vectors
# ---------------------------------------------------------------------------------------

Write-Section 'Motion vectors'

$providerNames = @{
    '0' = 'texMotionVectors (shared: DRME, qUINT, dh_uber_motion, ...)'
    '1' = 'iMMERSE Launchpad (Deferred::MotionVectorsTex)'
    '2' = 'VORT (MotVectTexVort)'
    '3' = 'LumeniteFX Kernel (Kernel::tFlow)'
    '4' = 'LumeniteFX QuantMotion (QuantMotion::tFlow)'
}
$providerFx = @{
    '1' = 'MartysMods_LAUNCHPAD.fx'
    '2' = 'vort_Motion.fx'
    '3' = 'lumenite_Kernel.fx'
    '4' = 'lumenite_QuantMotion.fx'
}

# Which preset is actually in use: ReShade.ini's [GENERAL] PresetPath names it, and it is
# not required to be called ReShadePreset.ini or to sit next to the game (issue #50).
$preset = $null
if ($reshadeIni) {
    $presetPath = Get-IniValue $reshadeIni 'GENERAL' 'PresetPath'
    if ($presetPath) {
        $presetPath = $presetPath.Trim()
        if (-not [System.IO.Path]::IsPathRooted($presetPath)) {
            $presetPath = Join-Path (Split-Path -Parent $reshadeIni) $presetPath
        }
        if (Test-FileHere $presetPath) { $preset = (Resolve-Path -LiteralPath $presetPath).Path }
    }
}
if (-not $preset) { $preset = Find-FileIn $gameDir 'ReShadePreset.ini' }
if (-not $preset) {
    Report -Status 'Fail' -Text 'ReShadePreset.ini is missing.' `
           -Detail 'The motion-vector provider is stored there, per effect. Without it DLSS5_Feed.fx falls back to provider 0 and no technique is enabled.' `
           -Action 'Copy the project''s ReShadePreset.ini template into the game folder.'
}
else {
    $lines = Read-LinesSafe $preset
    if ($null -eq $lines) {
        Report -Status 'Warn' -Text 'ReShadePreset.ini could not be read.' -Detail $preset
    }
    else {
        $techniques = Get-IniValue $preset '' 'Techniques'

        # ReShade assembles an effect's preprocessor definitions from THREE levels, in
        # runtime.cpp's load_effect: ReShade.ini's [GENERAL] PreprocessorDefinitions is the
        # base list, the preset's ROOT (section-less) PreprocessorDefinitions applies to
        # every effect, and a per-effect [DLSS5_Feed.fx] section overrides both. This check
        # used to read only the third, call a missing section a FAILURE and call the other
        # two levels mistakes -- so it declared "this install will not work as it stands" on
        # installs whose own add-on log read "-> Lumenite_Kernel (enabled)" (issue #50).
        # Most specific first; all three are valid places to set it.
        $mvLevels = @(
            @{ Where = 'the [DLSS5_Feed.fx] section of the preset'
               Value = (Get-PreprocessorDefinition (Get-IniValue $preset 'DLSS5_Feed.fx' 'PreprocessorDefinitions') 'DLSS5_MV_PROVIDER') },
            @{ Where = "the preset's root PreprocessorDefinitions (applies to every effect)"
               Value = (Get-PreprocessorDefinition (Get-IniValue $preset '' 'PreprocessorDefinitions') 'DLSS5_MV_PROVIDER') },
            @{ Where = "ReShade.ini's [GENERAL] PreprocessorDefinitions (the base list)"
               Value = $(if ($reshadeIni) { Get-PreprocessorDefinition (Get-IniValue $reshadeIni 'GENERAL' 'PreprocessorDefinitions') 'DLSS5_MV_PROVIDER' } else { $null }) }
        )
        $set      = @($mvLevels | Where-Object { $_.Value })
        $provider = $null
        $mvWhere  = $null
        if ($set.Count -gt 0) { $provider = $set[0].Value; $mvWhere = $set[0].Where }

        # The add-on's own log is better evidence than any of the three: it reports what
        # ReShade actually resolved at compile time, after all the overriding is done.
        if ($script:FeedLogProvider -and $script:FeedLogProvider -ne $provider) {
            Report -Status 'Na' -Text ('dlss5-feed.log resolved DLSS5_MV_PROVIDER=' + $script:FeedLogProvider + ' at compile time.') `
                   -Detail 'That is what the effect was actually built with in the last run. Where it disagrees with the ini files, believe the log.'
            $provider = $script:FeedLogProvider
            $mvWhere  = 'the add-on''s own log line from the last run'
        }

        if ($provider) {
            if ($providerNames.ContainsKey($provider)) { $pn = $providerNames[$provider] } else { $pn = 'unknown provider id' }
            Report -Status 'Ok' -Text ('DLSS5_MV_PROVIDER=' + $provider + ' -- ' + $pn) -Detail ('Read from ' + $mvWhere + '.')
            # More than one level carrying a value is legal, and the most specific wins --
            # but it is worth naming, because editing the losing one changes nothing.
            if ($set.Count -gt 1) {
                $shadowed = @($set | Select-Object -Skip 1 | ForEach-Object { $_.Where + ' = ' + $_.Value }) -join '; '
                Report -Status 'Na' -Text 'DLSS5_MV_PROVIDER is set at more than one level.' `
                       -Detail ('The most specific wins, so ' + $mvWhere + ' is the one in force. Also set, and overridden: ' + $shadowed)
            }
        }
        else {
            Report -Status 'Warn' -Text 'DLSS5_MV_PROVIDER is not set anywhere -- the effect defaults to provider 0.' `
                   -Detail 'Checked the [DLSS5_Feed.fx] section of the preset, the preset''s root PreprocessorDefinitions and ReShade.ini''s [GENERAL] PreprocessorDefinitions; any of the three is valid. Provider 0 reads the shared texMotionVectors texture, so it only works if another effect (DRME, qUINT, dh_uber_motion) writes it. The recommended setting is DLSS5_MV_PROVIDER=3 (LumeniteFX Kernel).'
            $provider = '0'
        }

        # Techniques= line: is DLSS5_Feed on, and is the provider's own effect on too?
        if (-not $techniques) {
            Report -Status 'Fail' -Text 'ReShadePreset.ini has no Techniques= line -- no effect is enabled.' `
                   -Action 'Enable DLSS5_Feed (and the motion-vector provider effect) in the ReShade overlay.'
        }
        else {
            $techList = @($techniques -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
            $feedIdx = -1
            for ($i = 0; $i -lt $techList.Count; $i++) { if ($techList[$i] -match '(?i)DLSS5_Feed\.fx$') { $feedIdx = $i; break } }

            if ($feedIdx -lt 0) {
                Report -Status 'Fail' -Text 'DLSS5_Feed is not in the enabled Techniques= list.' `
                       -Detail ('Techniques=' + $techniques) `
                       -Action 'Tick "DLSS 5 Feed" in the ReShade overlay (Home key) and it will be saved into the preset.'
            }
            else {
                Report -Status 'Ok' -Text ('DLSS5_Feed is enabled (position ' + ($feedIdx + 1) + ' of ' + $techList.Count + ' in Techniques=).')
            }

            if ($provider -and $providerFx.ContainsKey($provider)) {
                $wantFx = $providerFx[$provider]
                $provIdx = -1
                for ($i = 0; $i -lt $techList.Count; $i++) { if ($techList[$i] -match ('(?i)' + [regex]::Escape($wantFx) + '$')) { $provIdx = $i; break } }

                if ($provIdx -lt 0) {
                    Report -Status 'Fail' -Text ('Provider ' + $provider + ' is selected but no technique from ' + $wantFx + ' is enabled.') `
                           -Detail ('Techniques=' + $techniques + "`nThe provider effect must run, and must run BEFORE DLSS5_Feed, or the feeder gets an empty motion-vector texture.") `
                           -Action ('Enable the ' + $wantFx + ' technique in the ReShade overlay, above DLSS 5 Feed.')
                }
                elseif ($feedIdx -ge 0 -and $provIdx -gt $feedIdx) {
                    Report -Status 'Fail' -Text ('The ' + $wantFx + ' technique runs AFTER DLSS5_Feed.') `
                           -Detail 'DLSS5_Feed would read last frame''s (or an empty) motion-vector texture.' `
                           -Action ('Drag ' + $wantFx + ' above DLSS 5 Feed in the ReShade technique list.')
                }
                else {
                    Report -Status 'Ok' -Text ($wantFx + ' is enabled and runs before DLSS5_Feed.')
                }
            }
            elseif ($provider -eq '0') {
                $writers = @('MotionEstimation.fx', 'qUINT_motionvectors.fx', 'dh_uber_motion.fx', 'vort_Motion.fx')
                $found = @()
                foreach ($w in $writers) { foreach ($t in $techList) { if ($t -match ('(?i)' + [regex]::Escape($w) + '$')) { $found += $w } } }
                if ($found.Count -gt 0) {
                    Report -Status 'Ok' -Text ('Provider 0: a texMotionVectors writer is enabled (' + (($found | Select-Object -Unique) -join ', ') + ').')
                }
                else {
                    Report -Status 'Fail' -Text 'Provider 0 is selected but no known texMotionVectors writer is enabled.' `
                           -Detail ('Techniques=' + $techniques + "`nProvider 0 only reads the shared texture -- something has to fill it.") `
                           -Action 'Enable a motion-vector effect (DRME/MotionEstimation.fx, qUINT, dh_uber_motion), or switch to DLSS5_MV_PROVIDER=3 with lumenite_Kernel.fx.'
                }
            }
        }
    }
}

# ---------------------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------------------

Write-Host ''
Write-Chunk ('  ' + ([string][char]0x2500) * 68) 'Green'

Write-Chunk '  ' $null -NoNewline
Write-Chunk ($script:CountOk.ToString() + ' OK') 'Green' -NoNewline
Write-Chunk '   ' $null -NoNewline
Write-Chunk ($script:CountWarn.ToString() + ' warning' + $(if ($script:CountWarn -eq 1) { '' } else { 's' })) 'Yellow' -NoNewline
Write-Chunk '   ' $null -NoNewline
Write-Chunk ($script:CountFail.ToString() + ' failure' + $(if ($script:CountFail -eq 1) { '' } else { 's' })) 'Red'

if ($script:CountFail -gt 0) {
    Write-Chunk '  Verdict: this install will not work as it stands.' 'Red'
    Write-Host ''
    Write-Chunk '  Do this:' 'White'
    $i = 1
    foreach ($a in $script:Actions) {
        $first = $true
        foreach ($line in ($a -split "`n")) {
            if (-not $line.Trim()) { continue }
            if ($first) { Write-Chunk ('   ' + $i + '. ' + $line.Trim()) 'DarkYellow'; $first = $false }
            else { Write-Chunk ('      ' + $line.Trim()) 'DarkYellow' }
        }
        $i++
    }
}
elseif ($script:CountWarn -gt 0) {
    Write-Chunk '  Verdict: the install looks complete, but read the warnings above.' 'Yellow'
}
else {
    Write-Chunk '  Verdict: everything checks out.' 'Green'
}

Write-Host ''

if ($script:CountFail -gt 0) { Exit-Verifier 1 }
Exit-Verifier 0
