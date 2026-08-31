[CmdletBinding()]
param(
    [ValidateSet('Install', 'ConfigureRuntime', 'Disable', 'Validate', 'OpenRHI')]
    [string]$Action = 'Install',

    [ValidateSet('Auto', 'DLSS45', 'DLSS5', 'DLSS5Extended')]
    [string]$Profile = 'Auto',

    [string]$GameRoot = $PSScriptRoot,
    [string]$SettingsPath,
    [string]$DlssRuntime,
    [string]$DlssNrRuntime,
    [string]$RenoDxAddon,
    [string]$GraphicsCacheRoot,
    [string]$ReShadeSetup,
    [string]$ReShadeFfx,
    [string]$VortShadersZip,
    [switch]$AcceptDependencyLicenses,
    [switch]$AcceptRuntimeLicenses,
    [switch]$AllowUnsignedNvidiaRuntime,
    [switch]$ForceDownload
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Write-Status([string]$Message) {
    Write-Host "[CK3 DLSS] $Message"
}

function Resolve-CK3Root([string]$Candidate) {
    $resolved = [IO.Path]::GetFullPath($Candidate.TrimEnd([char[]]@(92, 47)))
    if (Test-Path -LiteralPath (Join-Path $resolved 'binaries\\ck3.exe') -PathType Leaf) { return $resolved }
    if ((Split-Path -Leaf $resolved) -ieq 'binaries' -and
        (Test-Path -LiteralPath (Join-Path $resolved 'ck3.exe') -PathType Leaf)) {
        return (Split-Path -Parent $resolved)
    }
    throw "binaries\\ck3.exe was not found below '$resolved'. Extract the package into the Crusader Kings III root folder."
}

function Get-DefaultSettingsPath {
    $documents = [Environment]::GetFolderPath([Environment+SpecialFolder]::MyDocuments)
    return Join-Path $documents 'Paradox Interactive\\Crusader Kings III\\pdx_settings.txt'
}

function Read-SettingsDocument([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $bytes = [IO.File]::ReadAllBytes($Path)
    $encoding = [Text.UTF8Encoding]::new($false)
    $preambleLength = 0
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $encoding = [Text.UTF8Encoding]::new($true)
        $preambleLength = 3
    }
    $text = $encoding.GetString($bytes, $preambleLength, $bytes.Length - $preambleLength)
    return [pscustomobject]@{ Text = $text; Encoding = $encoding }
}

function Get-CK3Renderer([string]$Path) {
    $document = Read-SettingsDocument $Path
    if (-not $document) { return $null }
    $pattern = '(?ms)"renderer"\s*=\s*\{.*?\bvalue\s*=\s*"([^"]*)"'
    $matches = [regex]::Matches($document.Text, $pattern)
    if ($matches.Count -ne 1) {
        throw "Expected one Graphics renderer entry in '$Path', found $($matches.Count). Open the Paradox launcher once and retry."
    }
    return $matches[0].Groups[1].Value
}

function Set-CK3Renderer(
    [string]$Path,
    [ValidateSet('Vulkan', 'DX11')][string]$Renderer,
    [switch]$CreateBackup
) {
    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        $initial = @"
"Graphics"={
	"renderer"={
		version=0
		value="$Renderer"
	}
}
"@
        [IO.File]::WriteAllText($Path, $initial + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
        Write-Status "Created settings and selected $Renderer."
        return
    }

    $document = Read-SettingsDocument $Path
    $pattern = '(?ms)("renderer"\s*=\s*\{.*?\bvalue\s*=\s*")[^"]*(")'
    $matches = [regex]::Matches($document.Text, $pattern)
    if ($matches.Count -ne 1) {
        throw "Expected one Graphics renderer entry in '$Path', found $($matches.Count). Open the Paradox launcher once and retry."
    }
    $updated = [regex]::Replace($document.Text, $pattern, { param($match) $match.Groups[1].Value + $Renderer + $match.Groups[2].Value }, 1)
    if ($updated -ceq $document.Text) {
        Write-Status "Renderer is already $Renderer."
        return
    }

    if ($CreateBackup) {
        $backup = "$Path.ck3-dlss-backup"
        if (-not (Test-Path -LiteralPath $backup -PathType Leaf)) {
            Copy-Item -LiteralPath $Path -Destination $backup
            Write-Status "Backed up settings to '$backup'."
        }
    }
    [IO.File]::WriteAllText($Path, $updated, $document.Encoding)
    Write-Status "Changed CK3 renderer to $Renderer."
}

function Save-InstallState([string]$BinaryRoot, [string]$Path) {
    $statePath = Join-Path $BinaryRoot 'CK3-DLSS-INSTALL.json'
    if (Test-Path -LiteralPath $statePath -PathType Leaf) { return }
    $original = Get-CK3Renderer $Path
    $state = [ordered]@{
        SchemaVersion = 1
        InstalledUtc = [DateTime]::UtcNow.ToString('o')
        SettingsPath = [IO.Path]::GetFullPath($Path)
        OriginalRenderer = $original
        SettingsExisted = $null -ne $original
    }
    [IO.File]::WriteAllText($statePath, (($state | ConvertTo-Json -Depth 3) + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))
}

function Restore-OriginalRenderer([string]$BinaryRoot, [string]$Path) {
    $statePath = Join-Path $BinaryRoot 'CK3-DLSS-INSTALL.json'
    if (Test-Path -LiteralPath $statePath -PathType Leaf) {
        try { $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json }
        catch { throw "The install-state receipt is invalid: $($_.Exception.Message)" }
        $original = [string]$state.OriginalRenderer
        if ($original -in @('Vulkan', 'DX11')) {
            Set-CK3Renderer $Path $original
            Write-Status "Restored the renderer recorded at install time: $original."
        }
        else {
            Write-Status 'The settings file did not exist before install; leaving its renderer unchanged.'
        }
        Remove-Item -LiteralPath $statePath -Force
        return
    }

    $legacyBackup = "$Path.ck3-dlss-backup"
    if (Test-Path -LiteralPath $legacyBackup -PathType Leaf) {
        $original = Get-CK3Renderer $legacyBackup
        if ($original -notin @('Vulkan', 'DX11')) { throw "The legacy backup contains unsupported renderer '$original'." }
        Set-CK3Renderer $Path $original
        Write-Status "Restored renderer $original from the legacy settings backup."
        return
    }
    Write-Status 'No installer state exists; the saved renderer was left unchanged.'
}

function Test-BootstrapPackage([string]$Root) {
    $binaryRoot = Join-Path $Root 'binaries'
    $rootRequired = @('Graphics-Dependency-Setup.ps1', 'DLSS-Runtime-Setup.ps1')
    $binaryRequired = @(
        'dxgi.dll',
        'dlss-payload\\dlss5-feed.addon64',
        'ReShade.ini',
        'DLSS5-CK3.ini',
        'reshade-shaders\\Shaders\\DLSS5_Feed.fx',
        'dlss5-vulkan\\ReShade64.json',
        'dlss5-vulkan\\VkLayer_feed_vk.dll',
        'dlss5-vulkan\\VkLayer_feed_vk.json'
    )
    $missing = @()
    $missing += @($rootRequired | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Root $_) -PathType Leaf) })
    $missing += @($binaryRequired | Where-Object { -not (Test-Path -LiteralPath (Join-Path $binaryRoot $_) -PathType Leaf) } | ForEach-Object { "binaries\\$_" })
    if ($missing.Count) { throw "The bootstrap package is incomplete. Missing: $($missing -join ', ')" }
}

function Test-BasePackage([string]$Root) {
    Test-BootstrapPackage $Root
    $binaryRoot = Join-Path $Root 'binaries'
    $required = @(
        'dlss5-vulkan\\ReShade64.dll',
        'reshade-shaders\\Shaders\\ReShade.fxh',
        'third-party\\vort_Shaders\\Shaders\\vort_Motion.fx',
        'third-party\\vort_Shaders\\Shaders\\Includes\\vort_MotionUtils.fxh',
        'third-party\\vort_Shaders\\LICENSE',
        'CK3-DLSS-GRAPHICS.json'
    )
    $missing = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $binaryRoot $_) -PathType Leaf) })
    if ($missing.Count) { throw "Installed graphics dependencies are missing: $($missing -join ', ')" }

    $reshadeText = Get-Content -LiteralPath (Join-Path $binaryRoot 'ReShade.ini') -Raw
    if ($reshadeText -notmatch '(?im)^AddonPath=\.\\dlss-active\s*$') {
        throw 'ReShade.ini does not isolate add-ons in binaries\\dlss-active.'
    }
    if ($reshadeText -notmatch '(?im)vort_Shaders\\Shaders') {
        throw 'ReShade.ini does not include the installed VORT shader path.'
    }
    Write-Status 'Base package validation passed.'
}

function Invoke-GraphicsSetup([string]$GraphicsAction, [string]$Root) {
    $scriptPath = Join-Path $Root 'Graphics-Dependency-Setup.ps1'
    if (-not (Test-Path -LiteralPath $scriptPath -PathType Leaf)) { throw "Graphics setup script is missing: '$scriptPath'" }
    $arguments = @{ Action = $GraphicsAction; GameRoot = $Root }
    if ($GraphicsCacheRoot) { $arguments.CacheRoot = $GraphicsCacheRoot }
    if ($ReShadeSetup) { $arguments.ReShadeSetup = $ReShadeSetup }
    if ($ReShadeFfx) { $arguments.ReShadeFfx = $ReShadeFfx }
    if ($VortShadersZip) { $arguments.VortShadersZip = $VortShadersZip }
    if ($AcceptDependencyLicenses) { $arguments.AcceptDependencyLicenses = $true }
    if ($ForceDownload) { $arguments.ForceDownload = $true }
    & $scriptPath @arguments
}

function Invoke-RuntimeSetup([string]$RuntimeAction, [string]$Root) {
    $scriptPath = Join-Path $Root 'DLSS-Runtime-Setup.ps1'
    if (-not (Test-Path -LiteralPath $scriptPath -PathType Leaf)) { throw "Runtime setup script is missing: '$scriptPath'" }
    $arguments = @{ Action = $RuntimeAction; GameRoot = $Root }
    if ($RuntimeAction -eq 'Configure') { $arguments.Mode = $Profile }
    if ($DlssRuntime) { $arguments.DlssRuntime = $DlssRuntime }
    if ($DlssNrRuntime) { $arguments.DlssNrRuntime = $DlssNrRuntime }
    if ($RenoDxAddon) { $arguments.RenoDxAddon = $RenoDxAddon }
    if ($AcceptRuntimeLicenses) { $arguments.AcceptRuntimeLicenses = $true }
    if ($AllowUnsignedNvidiaRuntime) { $arguments.AllowUnsignedNvidiaRuntime = $true }
    if ($ForceDownload) { $arguments.ForceDownload = $true }
    & $scriptPath @arguments
}

try {
    $root = Resolve-CK3Root $GameRoot
    $binaryRoot = Join-Path $root 'binaries'
    if (-not $SettingsPath) { $SettingsPath = Get-DefaultSettingsPath }

    switch ($Action) {
        'Install' {
            Test-BootstrapPackage $root
            Invoke-GraphicsSetup 'Install' $root
            Test-BasePackage $root
            Invoke-RuntimeSetup 'Configure' $root
            Invoke-RuntimeSetup 'Status' $root
            Save-InstallState $binaryRoot $SettingsPath
            Set-CK3Renderer $SettingsPath 'Vulkan' -CreateBackup
            Write-Host ''
            Write-Status 'Ready. Start with "Launch CK3 with DLSS.cmd".'
            Write-Status 'In game, press Home and confirm vort_MotionEffects runs above DLSS 5 Feed.'
        }
        'ConfigureRuntime' {
            Invoke-GraphicsSetup 'Status' $root
            Test-BasePackage $root
            Invoke-RuntimeSetup 'Configure' $root
            Write-Status 'Runtime profile changed. CK3 renderer settings were not modified.'
        }
        'Disable' {
            Restore-OriginalRenderer $binaryRoot $SettingsPath
            Write-Status 'Portable Vulkan layers are inactive when CK3 is launched normally.'
        }
        'Validate' {
            Invoke-GraphicsSetup 'Status' $root
            Test-BasePackage $root
            Invoke-RuntimeSetup 'Status' $root
            $renderer = Get-CK3Renderer $SettingsPath
            if ($renderer -ne 'Vulkan') { throw "CK3's saved renderer is '$renderer', not Vulkan. Run Install CK3 DLSS.cmd." }
            Write-Status "Renderer validated: $renderer ($SettingsPath)"
        }
        'OpenRHI' { Invoke-RuntimeSetup 'OpenRHI' $root }
    }
    exit 0
}
catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

