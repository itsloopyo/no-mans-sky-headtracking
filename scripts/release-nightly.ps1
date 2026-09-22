# Thin shim. Determine version, delegate to the shared publisher.
# See cameraunlock-core/powershell/NightlyRelease.psm1 for what it does.

[CmdletBinding()]
param(
    [switch]$AllowDirty
)

$ErrorActionPreference = 'Stop'

$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot '..')

Import-Module (Join-Path $ProjectRoot 'cameraunlock-core\powershell\NightlyRelease.psm1') -Force

$manifestFile = Join-Path $ProjectRoot 'launcher-manifest.json'
$versionMatch = Select-String -Path $manifestFile -Pattern '^\s*"version":\s*"([^"]+)"'
if (-not $versionMatch) {
    throw "Could not extract version from $manifestFile"
}
$version = $versionMatch.Matches[0].Groups[1].Value

Publish-NightlyBuild `
    -ModId 'no-mans-sky' `
    -ModName 'NoMansSkyHeadTracking' `
    -Version $version `
    -ProjectRoot $ProjectRoot `
    -AllowDirty:$AllowDirty
