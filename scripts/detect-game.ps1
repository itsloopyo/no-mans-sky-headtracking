#!/usr/bin/env pwsh
# Locate No Man's Sky install directory. Delegates to the shared
# games.json-backed lookup in cameraunlock-core.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir

Import-Module (Join-Path $projectRoot 'cameraunlock-core\powershell\GamePathDetection.psm1') -Force

$gamePaths = @(Find-AllGamePaths -GameId 'no-mans-sky')
if ($gamePaths.Count -eq 0) {
    Write-Error "Could not find No Man's Sky installation. Set NO_MANS_SKY_PATH or pass the path explicitly."
    exit 1
}

foreach ($path in $gamePaths) {
    Write-Host "Found: $path" -ForegroundColor Green
}
$gamePaths
