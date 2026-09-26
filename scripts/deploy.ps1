#!/usr/bin/env pwsh
#Requires -Version 5.1
# Dev-time deploy: build output -> game Binaries dir. The shim is named
# XINPUT9_1_0.dll; NMS.exe imports XInputGetState/XInputSetState from
# it and the app directory wins over System32, so our copy is loaded at
# startup. No local original to back up - the genuine DLL lives in
# System32 and the proxy forwards there at runtime.
#
# Every copy of the game on the machine is deployed to, not the first one
# detection returns. Owning the Steam copy and the Game Pass copy at once is
# ordinary, and deploying to one while launching the other is a silent failure:
# the build is correct, the fix does not appear, and the time goes into
# re-debugging something that was never broken.
#
# No config is copied: the mod creates CameraUnlock.ini at its first start,
# importing HeadTracking.ini where an older build left one.

param(
    [Parameter(Mandatory=$true, Position=0)]
    [ValidateSet('Debug','Release')]
    [string]$Configuration,

    [Parameter(Mandatory=$false, Position=1)]
    [string]$GivenPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir

Import-Module (Join-Path $projectRoot 'cameraunlock-core\powershell\GamePathDetection.psm1') -Force

$buildOutput = Join-Path $projectRoot "bin\$Configuration"
$modDllName  = 'XINPUT9_1_0.dll'

$builtDll = Join-Path $buildOutput $modDllName
if (-not (Test-Path $builtDll)) {
    throw "Build artifact not found at: $builtDll. Run 'pixi run build-release' first."
}

if ($GivenPath) {
    $gamePaths = @($GivenPath)
} else {
    $gamePaths = @(Find-AllGamePaths -GameId 'no-mans-sky')
}
if ($gamePaths.Count -eq 0) {
    throw "Could not locate No Man's Sky. Pass -GivenPath or set NO_MANS_SKY_PATH."
}

$deployed = 0
$failed = @()
foreach ($gamePath in $gamePaths) {
    # The Game Pass build keeps the same Binaries layout, one level deeper:
    # detection returns <drive>:\XboxGames\No Man's Sky\Content, so the join
    # below lands on Content\Binaries without a special case.
    $exeDir = Join-Path $gamePath 'Binaries'
    if (-not (Test-Path $exeDir)) {
        Write-Host "Skipped (no Binaries folder): $gamePath" -ForegroundColor Yellow
        continue
    }

    # One copy having the game open must not cost the others their deploy - the
    # shim is loaded for the life of the process, so a running game locks it and
    # the usual cause is the copy you are NOT testing.
    try {
        Copy-Item -Path $builtDll -Destination (Join-Path $exeDir $modDllName) -Force -ErrorAction Stop
    } catch {
        Write-Host "FAILED: $exeDir" -ForegroundColor Red
        Write-Host "  $($_.Exception.Message)" -ForegroundColor Red
        Write-Host "  If that copy of the game is running, close it and deploy again." -ForegroundColor Red
        $failed += $exeDir
        continue
    }

    Write-Host ""
    Write-Host "Deployed to: $exeDir" -ForegroundColor Green
    Write-Host "  XINPUT9_1_0.dll  (mod shim)"
    $deployed++
}

Write-Host ""
if ($deployed -eq 0) {
    throw "Found $($gamePaths.Count) game path(s) but deployed to none of them."
}
Write-Host "Deployed to $deployed of $($gamePaths.Count) installation(s)." -ForegroundColor Green
if ($failed.Count -gt 0) {
    throw "$($failed.Count) installation(s) were not updated: $($failed -join ', ')"
}
