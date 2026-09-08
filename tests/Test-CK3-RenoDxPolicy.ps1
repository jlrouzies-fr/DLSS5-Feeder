[CmdletBinding()]
param()
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repoRoot = Split-Path -Parent $PSScriptRoot
$runtimeScript = Join-Path $repoRoot 'ck3-package\DLSS-Runtime-Setup.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($runtimeScript, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
# Load function definitions without executing the installer's Configure entry point.
$functions = $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $false)
foreach ($fn in $functions) { . ([scriptblock]::Create($fn.Extent.Text)) }
$script:Headers = @{ 'User-Agent' = 'CK3-DLSS-Compatibility-Test' }
$ForceDownload = $false
$AllowUnsignedNvidiaRuntime = $false
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('ck3-renodx-policy-' + [guid]::NewGuid().ToString('N'))
$expectedDll = '9150097cdee2953cdc9894d2e5606ea5100e6c8f95fc7bb1b407328b4391a07a'
try {
    Ensure-Directory $testRoot
    $component = Get-RenoDxAddon $testRoot ''
    if ($component.Version -ne '4.55' -or $component.Info.Sha256 -ne $expectedDll) { throw 'Online consumer is not the reviewed v4.55 build.' }
    if ($component.Source -notmatch '/renodx-dlss5-4\.55/') { throw 'Online acquisition used an unpinned source.' }
    $cache = Join-Path $testRoot 'renodx-dlss5_4.55.addon64'
    # A valid cache must work offline; no manifest or release-list lookup is allowed.
    function Get-RemoteJson { throw 'Unexpected release/manifest lookup' }
    function Save-Download { throw 'Unexpected download for valid cache' }
    $cached = Get-RenoDxAddon $testRoot ''
    if ($cached.Info.Sha256 -ne $expectedDll) { throw 'Valid cached consumer changed.' }
    # A poisoned cache must request a replacement rather than trusting its filename.
    [IO.File]::WriteAllText($cache, 'poisoned-cache')
    $rejected = $false
    try { $null = Get-RenoDxAddon $testRoot '' } catch { $rejected = $_.Exception.Message -eq 'Unexpected download for valid cache' }
    if (-not $rejected) { throw 'Corrupt cache was accepted or failed to request a replacement.' }
    # A downloaded archive must pass its pin before any extraction.
    function Save-Download([string]$Uri, [string]$Destination, [string]$Label) { [IO.File]::WriteAllText($Destination, 'tampered-archive') }
    function Copy-ArchivePayload { throw 'Unverified archive reached extraction' }
    $rejected = $false
    try { $null = Get-RenoDxAddon $testRoot '' } catch { $rejected = $_.Exception.Message -eq 'The RenoDX archive did not match its reviewed SHA-256.' }
    if (-not $rejected) { throw 'Unverified archive was not rejected before extraction.' }
    Write-Host 'RenoDX policy passed: real v4.55 download/hash, offline cache, poisoned cache, archive rejection.' -ForegroundColor Green
}
finally {
    $resolved = [IO.Path]::GetFullPath($testRoot)
    $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\ck3-renodx-policy-'
    if (-not $resolved.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test cleanup path.' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}