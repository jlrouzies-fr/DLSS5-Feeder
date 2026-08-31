[CmdletBinding()]
param(
    [string]$ZipPath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'release\CK3-DLSS-Complete-Test.zip')
)

$ErrorActionPreference = 'Stop'

function Invoke-Installer {
    param(
        [Parameter(Mandatory)][string]$Installer,
        [Parameter(Mandatory)][string]$Fixture,
        [Parameter(Mandatory)][string]$Settings,
        [Parameter(Mandatory)][string]$Action,
        [Parameter(Mandatory)][string]$Profile
    )

    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $Installer,
        '-Action', $Action, '-Profile', $Profile,
        '-GameRoot', $Fixture, '-SettingsPath', $Settings,
        '-AcceptDependencyLicenses', '-AcceptRuntimeLicenses'
    )
    & powershell.exe @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Action/$Profile failed with exit code $LASTEXITCODE."
    }
}

function Assert-Profile {
    param(
        [Parameter(Mandatory)][string]$Fixture,
        [Parameter(Mandatory)][string]$Profile,
        [Parameter(Mandatory)][bool]$ExpectNeuralRendering,
        [Parameter(Mandatory)][int]$ExpectedPreset
    )

    $active = Join-Path $Fixture 'binaries\dlss-active'
    $state = Get-Content -LiteralPath (Join-Path $active 'CK3-DLSS-RUNTIME.json') -Raw | ConvertFrom-Json
    if ($state.Profile -ne $Profile) { throw "Expected profile $Profile; found $($state.Profile)." }

    $config = Get-Content -LiteralPath (Join-Path $active 'dlss5-feed.cfg') -Raw
    if ($config -notmatch "(?m)^preset=$ExpectedPreset\s*$") {
        throw "$Profile did not activate preset $ExpectedPreset."
    }

    foreach ($name in @('nvngx_dlssnr.dll', 'renodx-dlss5.addon64')) {
        $present = Test-Path -LiteralPath (Join-Path $active $name) -PathType Leaf
        if ($present -ne $ExpectNeuralRendering) { throw "$Profile has incorrect $name state." }
    }

    $payload = Join-Path $Fixture "binaries\dlss-payload\runtimes\$Profile"
    $activeDlss = (Get-FileHash -LiteralPath (Join-Path $active 'nvngx_dlss.dll') -Algorithm SHA256).Hash
    $payloadDlss = (Get-FileHash -LiteralPath (Join-Path $payload 'nvngx_dlss.dll') -Algorithm SHA256).Hash
    if ($activeDlss -ne $payloadDlss) { throw "$Profile copied the wrong base DLSS runtime." }

    if ($ExpectNeuralRendering) {
        $activeNr = (Get-FileHash -LiteralPath (Join-Path $active 'nvngx_dlssnr.dll') -Algorithm SHA256).Hash
        $payloadNr = (Get-FileHash -LiteralPath (Join-Path $payload 'nvngx_dlssnr.dll') -Algorithm SHA256).Hash
        if ($activeNr -ne $payloadNr) { throw "$Profile copied the wrong neural-rendering runtime." }
    }
}

$resolvedZip = (Resolve-Path -LiteralPath $ZipPath).Path
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([char[]]@(92, 47))
$fixture = Join-Path $tempBase ("ck3-dlss-bundle-test-{0}" -f [guid]::NewGuid().ToString('N'))
$resolvedFixture = [IO.Path]::GetFullPath($fixture)
if (-not $resolvedFixture.StartsWith($tempBase + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe fixture path '$resolvedFixture'."
}

try {
    Expand-Archive -LiteralPath $resolvedZip -DestinationPath $resolvedFixture
    foreach ($relative in @(
        'binaries\reshade-shaders\Shaders\Lilium\lilium__tone_mapping.fx',
        'binaries\reshade-shaders\Shaders\Lilium\lilium__inverse_tone_mapping.fx',
        'binaries\reshade-shaders\Shaders\Lilium\lilium__include\include_main.fxh',
        'binaries\reshade-shaders\Textures\Lilium\lilium__blue_noise_64x64.png',
        'THIRD-PARTY-LICENSES\Lilium-GPL-3.0.txt'
    )) {
        if (-not (Test-Path -LiteralPath (Join-Path $resolvedFixture $relative) -PathType Leaf)) { throw "Complete ZIP is missing $relative." }
    }

    $installer = Join-Path $resolvedFixture 'DLSS5-CK3.ps1'
    $settings = Join-Path $resolvedFixture 'pdx_settings.txt'
    Copy-Item -LiteralPath (Join-Path $resolvedFixture 'binaries\dlss-payload\dlss5-feed.addon64') `
        -Destination (Join-Path $resolvedFixture 'binaries\ck3.exe')

    $settingsText = @'
"Graphics"={
    "renderer"={
        version=0
        value="DX11"
    }
}
'@
    [IO.File]::WriteAllText($settings, $settingsText + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))

    Invoke-Installer $installer $resolvedFixture $settings Install DLSS45
    Invoke-Installer $installer $resolvedFixture $settings Validate DLSS45
    Assert-Profile $resolvedFixture DLSS45 $false 13
    if ((Get-Content -LiteralPath $settings -Raw) -notmatch 'value="Vulkan"') { throw 'Install did not select Vulkan.' }

    foreach ($profile in @('DLSS5', 'DLSS5Extended')) {
        Invoke-Installer $installer $resolvedFixture $settings ConfigureRuntime $profile
        Invoke-Installer $installer $resolvedFixture $settings Validate $profile
        Assert-Profile $resolvedFixture $profile $true 0
    }

    Invoke-Installer $installer $resolvedFixture $settings ConfigureRuntime DLSS45
    Invoke-Installer $installer $resolvedFixture $settings Validate DLSS45
    Assert-Profile $resolvedFixture DLSS45 $false 13

    Invoke-Installer $installer $resolvedFixture $settings Disable DLSS45
    if ((Get-Content -LiteralPath $settings -Raw) -notmatch 'value="DX11"') { throw 'Disable did not restore DX11.' }
    if (Test-Path -LiteralPath (Join-Path $resolvedFixture 'binaries\CK3-DLSS-INSTALL.json')) {
        throw 'Disable did not remove the install receipt.'
    }

    Write-Host 'Complete ZIP validation passed: DLSS45 -> DLSS5 -> DLSS5Extended -> DLSS45 -> Disable.' -ForegroundColor Green
}
finally {
    if (Test-Path -LiteralPath $resolvedFixture -PathType Container) {
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
    }
}
