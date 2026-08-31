[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('ck3-dlss-online-' + [guid]::NewGuid().ToString('N'))

function New-FakeX64Pe([string]$Path) {
    $parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $bytes = [byte[]]::new(8192)
    $bytes[0] = 0x4D
    $bytes[1] = 0x5A
    [BitConverter]::GetBytes([int]0x80).CopyTo($bytes, 0x3C)
    $bytes[0x80] = 0x50
    $bytes[0x81] = 0x45
    [BitConverter]::GetBytes([uint16]0x8664).CopyTo($bytes, 0x84)
    [IO.File]::WriteAllBytes($Path, $bytes)
}

try {
    $binaryRoot = Join-Path $tempRoot 'binaries'
    New-FakeX64Pe (Join-Path $binaryRoot 'dlss-payload\dlss5-feed.addon64')
    $scriptPath = Join-Path $repoRoot 'ck3-package\DLSS-Runtime-Setup.ps1'
    & $scriptPath -Action Configure -Mode DLSS45 -GameRoot $tempRoot -AcceptRuntimeLicenses

    $statePath = Join-Path $binaryRoot 'dlss-active\CK3-DLSS-RUNTIME.json'
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    if ($state.Profile -ne 'DLSS45') { throw 'Online setup did not activate DLSS45.' }
    if ($state.Components.Dlss.SignatureStatus -ne 'Valid') { throw 'Official DLSS runtime signature was not valid.' }
    if ([string]$state.Components.Dlss.Signer -notmatch '(?i)NVIDIA') { throw 'Official DLSS runtime signer was not NVIDIA.' }
    if ([string]$state.Components.Dlss.Source -notmatch '^https://raw\.githubusercontent\.com/NVIDIA/DLSS/') { throw 'DLSS runtime did not come from NVIDIA/DLSS.' }
    if (Test-Path -LiteralPath (Join-Path $binaryRoot 'dlss-active\renodx-dlss5.addon64')) { throw 'Online DLSS45 unexpectedly activated RenoDX.' }
    Write-Host "Official NVIDIA acquisition passed: $($state.Components.Dlss.Version), $($state.Components.Dlss.Sha256)"

    & $scriptPath -Action Configure -Mode DLSS5Extended -GameRoot $tempRoot -AcceptRuntimeLicenses
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    if ($state.Profile -ne 'DLSS5Extended' -or -not $state.Experimental) { throw 'Online setup did not activate the experimental profile.' }
    if ([string]$state.Components.DlssNr.Version -notmatch '(?i)SF') { throw 'Extended profile did not select the ShortFuse runtime.' }
    if ([string]$state.Components.DlssNr.Source -notmatch '^https://github\.com/RankFTW/rhi-repo/') { throw 'DLSS NR did not come from the RHI component repository.' }
    if ([string]$state.Components.RenoDx.Source -notmatch '^https://github\.com/RankFTW/rhi-repo/') { throw 'RenoDX did not come from the RHI component repository.' }
    if (-not (Test-Path -LiteralPath (Join-Path $binaryRoot 'dlss-active\nvngx_dlssnr.dll'))) { throw 'Extended profile did not activate DLSS NR.' }
    if (-not (Test-Path -LiteralPath (Join-Path $binaryRoot 'dlss-active\renodx-dlss5.addon64'))) { throw 'Extended profile did not activate RenoDX.' }
    Write-Host "RHI Extended acquisition passed: NR $($state.Components.DlssNr.Version), RenoDX $($state.Components.RenoDx.Version)"

    & $scriptPath -Action Configure -Mode DLSS45 -GameRoot $tempRoot -AcceptRuntimeLicenses
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    if ($state.Profile -ne 'DLSS45') { throw 'Final compatibility switch failed.' }
    if (Test-Path -LiteralPath (Join-Path $binaryRoot 'dlss-active\nvngx_dlssnr.dll')) { throw 'DLSS NR remained after the final compatibility switch.' }
    if (Test-Path -LiteralPath (Join-Path $binaryRoot 'dlss-active\renodx-dlss5.addon64')) { throw 'RenoDX remained after the final compatibility switch.' }

    Write-Host 'All online NVIDIA/RHI runtime acquisition tests passed.' -ForegroundColor Green
}
finally {
    if (Test-Path -LiteralPath $tempRoot) { Remove-Item -LiteralPath $tempRoot -Recurse -Force }
}

