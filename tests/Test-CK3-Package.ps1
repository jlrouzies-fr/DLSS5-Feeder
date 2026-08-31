[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$templateRoot = Join-Path $repoRoot 'ck3-package'
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('ck3-dlss-tests-' + [guid]::NewGuid().ToString('N'))

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

function Ensure-Directory([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { New-Item -ItemType Directory -Path $Path -Force | Out-Null }
}

function New-FakeX64Pe([string]$Path) {
    Ensure-Directory (Split-Path -Parent $Path)
    $bytes = [byte[]]::new(8192)
    $bytes[0] = 0x4D
    $bytes[1] = 0x5A
    [BitConverter]::GetBytes([int]0x80).CopyTo($bytes, 0x3C)
    $bytes[0x80] = 0x50
    $bytes[0x81] = 0x45
    [BitConverter]::GetBytes([uint16]0x8664).CopyTo($bytes, 0x84)
    [IO.File]::WriteAllBytes($Path, $bytes)
}

function Write-Utf8([string]$Path, [string]$Text) {
    Ensure-Directory (Split-Path -Parent $Path)
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Write-Settings([string]$Path, [string]$Renderer) {
    Write-Utf8 $Path @"
"Graphics"={
	"renderer"={
		version=0
		value="$Renderer"
	}
}
"@
}

function Write-GraphicsReceipt([string]$BinaryRoot) {
    $dll = Join-Path $BinaryRoot 'dlss5-vulkan\ReShade64.dll'
    $ffx = Join-Path $BinaryRoot 'reshade-shaders\Shaders\ReShade.fxh'
    $state = [ordered]@{
        SchemaVersion = 1
        Components = [ordered]@{
            ReShade = [ordered]@{ Version = 'test'; DllSha256 = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant() }
            ReShadeFfx = [ordered]@{ Sha256 = (Get-FileHash -LiteralPath $ffx -Algorithm SHA256).Hash.ToLowerInvariant() }
            Vort = [ordered]@{ Commit = '0000000000000000000000000000000000000000' }
        }
    }
    Write-Utf8 (Join-Path $BinaryRoot 'CK3-DLSS-GRAPHICS.json') ($state | ConvertTo-Json -Depth 6)
}

function Invoke-Setup([string[]]$Arguments, [int]$ExpectedExit = 0) {
    $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $script:fixtureRoot 'DLSS5-CK3.ps1') @Arguments 2>&1
    $code = $LASTEXITCODE
    $output | ForEach-Object { Write-Host $_ }
    if ($code -ne $ExpectedExit) { throw "Expected installer exit $ExpectedExit, got $code." }
}

try {
    $script:fixtureRoot = Join-Path $tempRoot 'Crusader Kings III'
    $binaryRoot = Join-Path $script:fixtureRoot 'binaries'
    Ensure-Directory $binaryRoot

    foreach ($name in @('DLSS5-CK3.ps1', 'DLSS-Runtime-Setup.ps1', 'Graphics-Dependency-Setup.ps1')) {
        Copy-Item -LiteralPath (Join-Path $templateRoot $name) -Destination (Join-Path $script:fixtureRoot $name)
    }
    Copy-Item -LiteralPath (Join-Path $templateRoot 'binaries\ReShade.ini') -Destination (Join-Path $binaryRoot 'ReShade.ini')
    Copy-Item -LiteralPath (Join-Path $templateRoot 'binaries\DLSS5-CK3.ini') -Destination (Join-Path $binaryRoot 'DLSS5-CK3.ini')
    Ensure-Directory (Join-Path $binaryRoot 'dlss5-vulkan')
    Copy-Item -LiteralPath (Join-Path $templateRoot 'binaries\dlss5-vulkan\ReShade64.json') -Destination (Join-Path $binaryRoot 'dlss5-vulkan\ReShade64.json') -Force

    New-FakeX64Pe (Join-Path $binaryRoot 'ck3.exe')
    New-FakeX64Pe (Join-Path $binaryRoot 'dlss-payload\dlss5-feed.addon64')
    foreach ($profile in @('DLSS45', 'DLSS5', 'DLSS5Extended')) {
        New-FakeX64Pe (Join-Path $binaryRoot "dlss-payload\runtimes\$profile\nvngx_dlss.dll")
    }
    foreach ($profile in @('DLSS5', 'DLSS5Extended')) {
        New-FakeX64Pe (Join-Path $binaryRoot "dlss-payload\runtimes\$profile\nvngx_dlssnr.dll")
    }
    New-FakeX64Pe (Join-Path $binaryRoot 'dlss-payload\runtimes\shared\renodx-dlss5.addon64')
    New-FakeX64Pe (Join-Path $binaryRoot 'dlss5-vulkan\ReShade64.dll')
    New-FakeX64Pe (Join-Path $binaryRoot 'dlss5-vulkan\VkLayer_feed_vk.dll')
    Write-Utf8 (Join-Path $binaryRoot 'dlss5-vulkan\VkLayer_feed_vk.json') '{"layer":{"name":"VK_LAYER_feed_vk"}}'
    Write-Utf8 (Join-Path $binaryRoot 'reshade-shaders\Shaders\DLSS5_Feed.fx') '// test feeder shader'
    Write-Utf8 (Join-Path $binaryRoot 'reshade-shaders\Shaders\ReShade.fxh') '// test ReShade header'
    Write-Utf8 (Join-Path $binaryRoot 'third-party\vort_Shaders\Shaders\vort_Motion.fx') '// test VORT'
    Write-Utf8 (Join-Path $binaryRoot 'third-party\vort_Shaders\Shaders\Includes\vort_MotionUtils.fxh') '// test VORT include'
    Write-Utf8 (Join-Path $binaryRoot 'third-party\vort_Shaders\LICENSE') 'MIT test license'
    Write-GraphicsReceipt $binaryRoot

    $settings = Join-Path $tempRoot 'pdx_settings.txt'
    Write-Settings $settings 'DX11'

    Write-Host 'TEST: RTX 3060 baseline installs with Model M neural reconstruction and no DLSS 5 extension'
    Invoke-Setup @(
        '-Action', 'Install', '-Profile', 'DLSS45', '-GameRoot', $script:fixtureRoot,
        '-SettingsPath', $settings, '-AcceptDependencyLicenses', '-AcceptRuntimeLicenses', '-AllowUnsignedNvidiaRuntime'
    )
    $active = Join-Path $binaryRoot 'dlss-active'
    $state = Get-Content -LiteralPath (Join-Path $active 'CK3-DLSS-RUNTIME.json') -Raw | ConvertFrom-Json
    Assert-True ($state.Profile -eq 'DLSS45') 'DLSS45 state was not selected.'
    Assert-True (Test-Path -LiteralPath (Join-Path $active 'nvngx_dlss.dll')) 'DLSS runtime was not activated.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $active 'nvngx_dlssnr.dll'))) 'DLSS NR must not be active in DLSS45 mode.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $active 'renodx-dlss5.addon64'))) 'RenoDX must not be active in DLSS45 mode.'
    $cfg = Get-Content -LiteralPath (Join-Path $active 'dlss5-feed.cfg') -Raw
    Assert-True ($cfg -match '(?m)^mode=2\r?$') 'DLSS45 must use the full NGX path.'
    Assert-True ($cfg -match '(?m)^preset=13\r?$') 'The RTX 3060 DLSS 4.5 test must select Model M.'
    Assert-True ($cfg -match '(?m)^create_delay=0\r?$') 'DLSS45 must skip the RenoDX create delay.'
    Assert-True ($cfg -match '(?m)^warmup_rebuild=0\r?$') 'DLSS45 must skip the RenoDX warm-up rebuild.'
    Assert-True ((Get-Content -LiteralPath $settings -Raw) -match 'value="Vulkan"') 'Install did not select Vulkan.'
    Assert-True ((Get-Content -LiteralPath "$settings.ck3-dlss-backup" -Raw) -match 'value="DX11"') 'The original renderer backup was not preserved.'

    Write-Host 'TEST: stock NR profile activates only its profile-specific pair plus shared RenoDX'
    Invoke-Setup @(
        '-Action', 'ConfigureRuntime', '-Profile', 'DLSS5', '-GameRoot', $script:fixtureRoot,
        '-SettingsPath', $settings, '-AcceptRuntimeLicenses', '-AllowUnsignedNvidiaRuntime'
    )
    $state = Get-Content -LiteralPath (Join-Path $active 'CK3-DLSS-RUNTIME.json') -Raw | ConvertFrom-Json
    Assert-True ($state.Profile -eq 'DLSS5') 'DLSS5 state was not selected.'
    Assert-True (Test-Path -LiteralPath (Join-Path $active 'nvngx_dlssnr.dll')) 'DLSS NR was not activated.'
    Assert-True (Test-Path -LiteralPath (Join-Path $active 'renodx-dlss5.addon64')) 'RenoDX was not activated.'

    Write-Host 'TEST: switching back removes NR files and full validation succeeds'
    Invoke-Setup @(
        '-Action', 'ConfigureRuntime', '-Profile', 'DLSS45', '-GameRoot', $script:fixtureRoot,
        '-SettingsPath', $settings, '-AcceptRuntimeLicenses', '-AllowUnsignedNvidiaRuntime'
    )
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $active 'nvngx_dlssnr.dll'))) 'DLSS NR remained after switching to DLSS45.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $active 'renodx-dlss5.addon64'))) 'RenoDX remained after switching to DLSS45.'
    Invoke-Setup @('-Action', 'Validate', '-GameRoot', $script:fixtureRoot, '-SettingsPath', $settings, '-AllowUnsignedNvidiaRuntime')

    Write-Host 'TEST: Disable restores the renderer recorded before installation'
    Invoke-Setup @('-Action', 'Disable', '-GameRoot', $script:fixtureRoot, '-SettingsPath', $settings)
    Assert-True ((Get-Content -LiteralPath $settings -Raw) -match 'value="DX11"') 'Disable did not restore DX11.'

    Write-Host 'TEST: failed runtime acquisition leaves the renderer untouched'
    $missing = Join-Path $tempRoot 'does-not-exist\nvngx_dlssnr.dll'
    Invoke-Setup @(
        '-Action', 'Install', '-Profile', 'DLSS5', '-GameRoot', $script:fixtureRoot,
        '-SettingsPath', $settings, '-DlssNrRuntime', $missing,
        '-AcceptDependencyLicenses', '-AcceptRuntimeLicenses', '-AllowUnsignedNvidiaRuntime'
    ) 1
    Assert-True ((Get-Content -LiteralPath $settings -Raw) -match 'value="DX11"') 'A failed runtime setup changed CK3 to Vulkan.'

    Write-Host 'TEST: an originally Vulkan user remains Vulkan after Disable'
    Write-Settings $settings 'Vulkan'
    Invoke-Setup @(
        '-Action', 'Install', '-Profile', 'DLSS45', '-GameRoot', $script:fixtureRoot,
        '-SettingsPath', $settings, '-AcceptDependencyLicenses', '-AcceptRuntimeLicenses', '-AllowUnsignedNvidiaRuntime'
    )
    Invoke-Setup @('-Action', 'Disable', '-GameRoot', $script:fixtureRoot, '-SettingsPath', $settings)
    Assert-True ((Get-Content -LiteralPath $settings -Raw) -match 'value="Vulkan"') 'Disable changed an originally Vulkan user to DX11.'

    Write-Host 'All isolated CK3 package tests passed.' -ForegroundColor Green
}
finally {
    if (Test-Path -LiteralPath $tempRoot -PathType Container) { Remove-Item -LiteralPath $tempRoot -Recurse -Force }
}

