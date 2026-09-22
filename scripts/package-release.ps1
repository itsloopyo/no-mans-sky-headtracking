#!/usr/bin/env pwsh
#Requires -Version 5.1
# Build two ZIPs in release/:
#   <ModName>-v<version>-installer.zip   for GitHub Releases (install.cmd + payload)
#   <ModName>-v<version>-nexus.zip       for Nexus (drop-in to game folder)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir

Import-Module (Join-Path $projectRoot "cameraunlock-core\powershell\ReleaseWorkflow.psm1") -Force

# Compress-Archive on Windows PowerShell 5.1 writes the platform separator into
# the entry names, so every path inside the ZIP comes out as `shared\find-game.ps1`.
# The ZIP spec mandates '/', and extractors that follow it (unzip, 7-Zip on
# Linux, several mod managers) then create one flat file literally named
# "shared\find-game.ps1" instead of the directory - and install.cmd fails its
# own layout check on the result. Write the entries ourselves with '/'.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function New-ZipFromDirectory {
    param(
        [Parameter(Mandatory=$true)][string]$SourceDir,
        [Parameter(Mandatory=$true)][string]$DestinationPath
    )
    if (Test-Path $DestinationPath) { Remove-Item $DestinationPath -Force }
    $root = (Resolve-Path $SourceDir).ProviderPath.TrimEnd('\')
    $zip  = [System.IO.Compression.ZipFile]::Open($DestinationPath, 'Create')
    try {
        foreach ($file in Get-ChildItem -Path $root -Recurse -File) {
            $entryName = $file.FullName.Substring($root.Length + 1).Replace('\', '/')
            [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $file.FullName, $entryName, 'Optimal')
        }
    } finally {
        $zip.Dispose()
    }
}

# The committed manifest is the authoritative copy of the seeded ini, and the
# blob inside the ZIP is a build product. Refreshing it here would ship a
# correct ZIP over a stale committed file, so drift fails the build instead.
Assert-ManifestSeedsMatchShipped -ManifestPath (Join-Path $projectRoot 'launcher-manifest.json') -ProjectRoot $projectRoot

$manifest = Get-Content (Join-Path $projectRoot 'launcher-manifest.json') -Raw | ConvertFrom-Json
# mod_info.name is already the PascalCase AssemblyName used for the ZIP names.
$modName  = $manifest.mod_info.name
$version  = $manifest.mod_info.version

$buildOutput = Join-Path $projectRoot 'bin\Release'
$modDll      = Join-Path $buildOutput 'XINPUT9_1_0.dll'
$configFile  = Join-Path $projectRoot 'HeadTracking.ini'

if (-not (Test-Path $modDll))     { throw "Missing build output: $modDll" }
if (-not (Test-Path $configFile)) { throw "Missing config: $configFile" }

$releaseDir = Join-Path $projectRoot 'release'
if (Test-Path $releaseDir) { Remove-Item -Recurse -Force $releaseDir }
New-Item -ItemType Directory -Path $releaseDir | Out-Null

$stagingRoot = Join-Path $releaseDir '_staging'
New-Item -ItemType Directory -Path $stagingRoot | Out-Null

# ---------------- Installer ZIP ----------------
$instStaging = Join-Path $stagingRoot 'installer'
New-Item -ItemType Directory -Path $instStaging | Out-Null

# Plugins payload (install-body-shim.cmd copies from .\plugins\ to game exe dir).
$pluginsDir = Join-Path $instStaging 'plugins'
New-Item -ItemType Directory -Path $pluginsDir | Out-Null
Copy-Item $modDll     -Destination (Join-Path $pluginsDir 'XINPUT9_1_0.dll')   -Force
Copy-Item $configFile -Destination (Join-Path $pluginsDir 'HeadTracking.ini') -Force

# install.cmd and uninstall.cmd are thin wrappers: the body they call lives in
# shared/ at the ZIP root, and without it the installer aborts at its own layout
# check and exits 1 on every run. Copy-SharedBundle stages every body there,
# alongside find-game.ps1, GamePathDetection.psm1 and games.json at the paths
# find-game.ps1 actually looks in.
Copy-SharedBundle -StagingDir $instStaging

# Top-level install/uninstall wrappers.
Copy-Item (Join-Path $projectRoot 'scripts\install.cmd')   -Destination $instStaging -Force
Copy-Item (Join-Path $projectRoot 'scripts\uninstall.cmd') -Destination $instStaging -Force

# Launcher manifest at the installer-ZIP root. This is the file lopari reads
# (delivery_mode "manifest" -> native deploy). files[].source paths are
# relative to this root (e.g. plugins/XINPUT9_1_0.dll).
Copy-Item (Join-Path $projectRoot 'launcher-manifest.json') -Destination $instStaging -Force

# Docs.
foreach ($doc in 'README.md','LICENSE','CHANGELOG.md','THIRD-PARTY-NOTICES.md') {
    Copy-Item (Join-Path $projectRoot $doc) -Destination $instStaging -Force
}

$installerZip = Join-Path $releaseDir "$modName-v$version-installer.zip"
New-ZipFromDirectory -SourceDir $instStaging -DestinationPath $installerZip
Write-Host "Created: $installerZip" -ForegroundColor Green

# ---------------- Nexus ZIP ----------------
$nexusStaging = Join-Path $stagingRoot 'nexus'
New-Item -ItemType Directory -Path $nexusStaging | Out-Null
Copy-Item $modDll     -Destination (Join-Path $nexusStaging 'XINPUT9_1_0.dll')   -Force
Copy-Item $configFile -Destination (Join-Path $nexusStaging 'HeadTracking.ini') -Force
Copy-Item (Join-Path $projectRoot 'README.md') -Destination $nexusStaging -Force
Copy-Item (Join-Path $projectRoot 'LICENSE')   -Destination $nexusStaging -Force

$nexusZip = Join-Path $releaseDir "$modName-v$version-nexus.zip"
# The Nexus ZIP is a binary distribution too: the licences of everything
# compiled into or bundled with the payload require their notices to travel
# with it, so LICENSE and THIRD-PARTY-NOTICES.md ship at its root.
foreach ($noticeDoc in @('LICENSE', 'THIRD-PARTY-NOTICES.md', 'README.md')) {
    $noticeSrc = Join-Path $projectRoot $noticeDoc
    if (-not (Test-Path $noticeSrc)) {
        throw "Required notice file not found: $noticeDoc. Every published ZIP is a binary distribution and must carry it."
    }
    Copy-Item $noticeSrc -Destination $nexusStaging -Force
    Write-Host "  $noticeDoc" -ForegroundColor Green
}
New-ZipFromDirectory -SourceDir $nexusStaging -DestinationPath $nexusZip
Write-Host "Created: $nexusZip" -ForegroundColor Green

Remove-Item -Recurse -Force $stagingRoot
