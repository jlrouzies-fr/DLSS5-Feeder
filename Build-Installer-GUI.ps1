[CmdletBinding()]
param(
    [string]$Destination = '',
    [switch]$SkipTests
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

if (-not $Destination) {
    $Destination = Join-Path $PSScriptRoot 'ck3-package\tools\CK3-DLSS-Installer'
}

$project = Join-Path $PSScriptRoot 'installer-gui'
$gradle = Join-Path $project 'gradlew.bat'
if (-not (Test-Path -LiteralPath $gradle -PathType Leaf)) {
    throw "Gradle wrapper not found: '$gradle'. Generate it from installer-gui before building."
}

$tasks = @()
if (-not $SkipTests) { $tasks += 'test' }
$tasks += 'createReleaseDistributable'

Write-Host "[installer GUI] Building and testing the Compose Desktop application..."
& $gradle --no-daemon -p $project @tasks
if ($LASTEXITCODE -ne 0) { throw "Installer GUI Gradle build failed with exit code $LASTEXITCODE." }

$source = Join-Path $project 'build\compose\binaries\main-release\app\CK3 DLSS Installer'
if (-not (Test-Path -LiteralPath $source -PathType Container)) {
    $matches = @(Get-ChildItem -LiteralPath (Join-Path $project 'build\compose\binaries\main-release\app') -Directory)
    if ($matches.Count -ne 1) { throw "Expected one Compose application image; found $($matches.Count)." }
    $source = $matches[0].FullName
}

$target = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $target -PathType Container) {
    Remove-Item -LiteralPath $target -Recurse -Force
}
New-Item -ItemType Directory -Path $target -Force | Out-Null
Get-ChildItem -LiteralPath $source -Force | Copy-Item -Destination $target -Recurse -Force

$executable = Join-Path $target 'CK3 DLSS Installer.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Compose application image did not contain '$executable'."
}

Write-Host "[installer GUI] Staged '$executable'."
