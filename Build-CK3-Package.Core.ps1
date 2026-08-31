[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$FeederAddon,
    [string]$FeedLayerZip,
    [string]$Dlss45Runtime,
    [string]$Dlss5Runtime,
    [string]$Dlss5NrRuntime,
    [string]$Dlss5ExtendedRuntime,
    [string]$Dlss5ExtendedNrRuntime,
    [string]$RenoDxAddon,
    [string]$StreamlineRuntimeDirectory,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'release\CK3-DLSS-Portable'),
    [switch]$AcknowledgeRuntimeRedistributionTerms,
    [switch]$AllowUnsignedTestArtifacts,
    [switch]$AllowUpstreamFeederForTesting,
    [switch]$KeepStagingDirectory
)

# Compatibility entry point. The portable implementation owns all build behavior.
$ErrorActionPreference = 'Stop'
$implementation = Join-Path $PSScriptRoot 'Build-CK3-Package.Portable.Core.ps1'
& $implementation @PSBoundParameters

