#!/usr/bin/env pwsh
#Requires -Version 5.1
<#
.SYNOPSIS
    Release workflow for No Man's Sky Head Tracking.

.DESCRIPTION
    1. Validate semver + git state.
    2. Regenerate CHANGELOG.md from conventional commits (via
       cameraunlock-core/powershell/ReleaseWorkflow.psm1).
    3. Bump version in launcher-manifest.json, CMakeLists.txt, pixi.toml,
       src/core/constants.h and the install.cmd MOD_VERSION line.
    4. Build + package the release.
    5. Commit the version + changelog as "Release v<version>".
    6. Create annotated tag v<version> and push it; CI picks up the tag and
       produces the GitHub release artifacts.

.PARAMETER Version
    Semver string (e.g. "1.0.0") or major|minor|patch.

.PARAMETER Force
    Ship a release even when there are no user-facing commits since the
    last tag (writes a maintenance changelog entry instead of aborting).

.EXAMPLE
    pixi run release 1.0.0
#>
param(
    [Parameter(Position = 0)]
    [string]$Version = '',
    # Ship a release even when there are no user-facing commits since the
    # last tag (writes a maintenance changelog entry instead of aborting).
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir
$manifestPath = Join-Path $projectRoot 'launcher-manifest.json'
$cmakePath    = Join-Path $projectRoot 'CMakeLists.txt'
$installCmdPath = Join-Path $projectRoot 'scripts\install.cmd'
$changelogPath = Join-Path $projectRoot 'CHANGELOG.md'

Import-Module (Join-Path $projectRoot 'cameraunlock-core\powershell\ReleaseWorkflow.psm1') -Force

# Mirrors New-ChangelogFromCommits' insertion so a -Force maintenance entry
# lands in the same place with the same shape.
function Add-MaintenanceChangelogEntry {
    param([string]$Path, [string]$NewVersion)
    $date = Get-Date -Format 'yyyy-MM-dd'
    $entry = "## [$NewVersion] - $date`n`n### Changed`n`n- Maintenance release (no user-facing changes).`n`n"
    $changelog = Get-Content $Path -Raw
    if ($changelog -match '(?s)(# Changelog.*?)(## \[)') {
        $changelog = $changelog -replace '(?s)(# Changelog.*?\n\n)', "`$1$entry"
    } else {
        $changelog = $changelog -replace '(?s)(# Changelog.*?\n)', "`$1$entry"
    }
    $changelog = $changelog.TrimEnd() + "`n"
    Set-Content $Path $changelog -NoNewline
}

function Get-ManifestVersion {
    $json = Get-Content $manifestPath -Raw
    if ($json -match '(?m)^\s*"version":\s*"([^"]+)"') {
        return $Matches[1]
    }
    throw "Could not read version from launcher-manifest.json"
}

Write-Host ''
Write-Host '=== No Man''s Sky Head Tracking Release ===' -ForegroundColor Cyan
Write-Host ''

$current = Get-ManifestVersion

if ([string]::IsNullOrWhiteSpace($Version)) {
    Write-Host "Current version: $current" -ForegroundColor Yellow
    Write-Host 'Usage: pixi run release <major|minor|patch|nightly|X.Y.Z>'
    exit 0
}

if ($Version -eq 'nightly') {
    & (Join-Path $PSScriptRoot 'release-nightly.ps1')
    exit $LASTEXITCODE
}

try {
    $Version = Resolve-ReleaseVersion -Argument $Version -CurrentVersion $current
} catch {
    Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

$tag = "v$Version"

$branch = (git rev-parse --abbrev-ref HEAD).Trim()
if ($branch -ne 'main') {
    Write-Host "Must be on main branch to release (currently on '$branch')" -ForegroundColor Red
    exit 1
}
if (-not (Test-CleanGitStatus)) {
    Write-Host 'Working tree has uncommitted changes - commit or stash first.' -ForegroundColor Red
    git status --short
    exit 1
}
if (Test-GitTagExists -Tag $tag) {
    Write-Host "Tag '$tag' already exists." -ForegroundColor Red
    exit 1
}

# THIRD-PARTY-NOTICES.md names the cameraunlock-core commit compiled into the
# release ZIPs, and bumping the submodule does not touch it. Packaging refuses
# to ship that mismatch, so a bump with no notices edit stopped the release
# here, or in CI once the tag had already been pushed. Re-sync it and let this
# release carry the correction. This runs AFTER the preconditions above: it can
# create a commit, and doing that before we know the branch, tree and tag are
# releasable left a stray commit behind on every aborted run.
& (Join-Path $projectRoot 'cameraunlock-core\scripts\sync-core-notices.ps1') -Repo $projectRoot
if ($LASTEXITCODE -ne 0) { throw "sync-core-notices.ps1 exited $LASTEXITCODE - fix THIRD-PARTY-NOTICES.md before releasing." }
& git -C $projectRoot diff --quiet -- THIRD-PARTY-NOTICES.md
if ($LASTEXITCODE -ne 0) {
    & git -C $projectRoot commit -q -m 'chore: record the cameraunlock-core commit this build compiles' -- THIRD-PARTY-NOTICES.md
    if ($LASTEXITCODE -ne 0) { throw "Could not commit the re-synced THIRD-PARTY-NOTICES.md." }
    Write-Host 'THIRD-PARTY-NOTICES.md re-synced to the pinned cameraunlock-core commit.' -ForegroundColor Yellow
}

Write-Host "Current version: $current" -ForegroundColor Gray
Write-Host "New version:     $Version" -ForegroundColor Green
Write-Host ''

# Step 1 - changelog (the gate that can fail). Generate it BEFORE mutating
# any version files so an abort here leaves the working tree clean instead
# of stranding a half-applied version bump with no tag.
Write-Host 'Generating CHANGELOG from commits...' -ForegroundColor Cyan
$hasTags = git tag -l 2>$null
if (-not $hasTags) {
    # PREPEND, never Set-Content. With no tags yet there is nothing to diff
    # against, but CHANGELOG.md is hand-written from the first commit onward and
    # overwriting it threw all of that away on the first versioned release - the
    # one release where it matters most, and silently, because nothing failed.
    $date = Get-Date -Format 'yyyy-MM-dd'
    $entry = "## [$Version] - $date`n`nFirst release.`n"
    if (Test-Path $changelogPath) {
        $existing = [System.IO.File]::ReadAllText($changelogPath)
        if ($existing -match [regex]::Escape("## [$Version]")) {
            Write-Host "CHANGELOG already carries a [$Version] section - left as written." -ForegroundColor Yellow
        } elseif ($existing -match '(?m)^# Changelog\r?\n') {
            # Instance Replace, not the static one. There is no static
            # Regex.Replace(String, String, String, Int32) overload: a trailing 1
            # binds to RegexOptions (= IgnoreCase) and the replace runs GLOBALLY,
            # so a second "# Changelog" anywhere in the file - a fenced example,
            # a quoted heading - would get its own copy of the release section.
            $headingRe = [regex]::new('(?m)^(# Changelog\r?\n\r?\n?)')
            $existing = $headingRe.Replace($existing, "`$1$entry`n", 1)
            [System.IO.File]::WriteAllText($changelogPath, $existing)
        } else {
            [System.IO.File]::WriteAllText($changelogPath, "# Changelog`n`n$entry`n$existing")
        }
    } else {
        Set-Content $changelogPath "# Changelog`n`n$entry"
    }
} else {
    try {
        $changelogArgs = @{
            ChangelogPath = $changelogPath
            Version       = $Version
            ArtifactPaths = @('src/', 'cameraunlock-core', 'scripts/')
        }
        New-ChangelogFromCommits @changelogArgs | Out-Null
    } catch {
        if (-not $Force) {
            Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
            Write-Host 'No user-facing changes to release. Re-run with -Force for a maintenance release.' -ForegroundColor Yellow
            exit 1
        }
        Write-Host 'No user-facing commits since last tag - writing maintenance entry (-Force).' -ForegroundColor Yellow
        Add-MaintenanceChangelogEntry -Path $changelogPath -NewVersion $Version
    }
}

# Step 2 - bump version in launcher-manifest.json. mod_info.version is the
# only 4-space-indented "version" key; the targeted replace preserves the
# hand-authored formatting.
Write-Host "Updating launcher-manifest.json to $Version..." -ForegroundColor Cyan
$json = Get-Content $manifestPath -Raw
$json = $json -replace '(?m)^(    "version": )"[^"]*"', "`$1`"$Version`""
[System.IO.File]::WriteAllText($manifestPath, $json, (New-Object System.Text.UTF8Encoding $false))

# CMakeLists.txt project() line.
Write-Host "Updating CMakeLists.txt to $Version..." -ForegroundColor Cyan
(Get-Content $cmakePath) -replace 'VERSION\s+\d+\.\d+\.\d+', "VERSION $Version" | Set-Content $cmakePath

# pixi.toml workspace version. Missed by every release until now, so it sat at
# 0.0.0 while the other four files moved and drifted further with each release.
Write-Host "Updating pixi.toml to $Version..." -ForegroundColor Cyan
$pixiPath = Join-Path $projectRoot 'pixi.toml'
$pixiRaw = [System.IO.File]::ReadAllText($pixiPath)
if ($pixiRaw -notmatch '(?m)^version = "\d+\.\d+\.\d+"') {
    throw "version line not found in $pixiPath"
}
# Instance Replace for the same reason as the changelog heading above.
$pixiVersionRe = [regex]::new('(?m)^version = "\d+\.\d+\.\d+"')
$pixiRaw = $pixiVersionRe.Replace($pixiRaw, "version = `"$Version`"", 1)
[System.IO.File]::WriteAllText($pixiPath, $pixiRaw)

# src/core/constants.h kModVersion. This is the version the mod prints at the
# top of every HeadTracking.log, which is the first line a bug report is read
# from, so a stale value here misroutes triage.
Write-Host "Updating src/core/constants.h to $Version..." -ForegroundColor Cyan
$constantsPath = Join-Path $projectRoot 'src/core/constants.h'
$constantsRaw = [System.IO.File]::ReadAllText($constantsPath)
$versionRe = [regex]::new('kModVersion = "\d+\.\d+\.\d+"')
if (-not $versionRe.IsMatch($constantsRaw)) {
    throw "kModVersion line not found in $constantsPath"
}
$constantsRaw = $versionRe.Replace($constantsRaw, "kModVersion = `"$Version`"", 1)
[System.IO.File]::WriteAllText($constantsPath, $constantsRaw)

# install.cmd MOD_VERSION. ReadAllText/WriteAllText preserve CRLF.
# Only install.cmd carries a version: the shared uninstall wrapper template has
# no MOD_VERSION line, so including it here threw "MOD_VERSION line not found"
# after the changelog and manifest had already been rewritten.
Write-Host "Updating install.cmd MOD_VERSION to $Version..." -ForegroundColor Cyan
$raw = [System.IO.File]::ReadAllText($installCmdPath)
if ($raw -notmatch 'set "MOD_VERSION=[^"]+"') {
    throw "MOD_VERSION line not found in $installCmdPath"
}
$raw = [regex]::Replace($raw, 'set "MOD_VERSION=[^"]+"', "set `"MOD_VERSION=$Version`"")
[System.IO.File]::WriteAllText($installCmdPath, $raw)

# Step 3 - build + package
Write-Host 'Building + packaging release...' -ForegroundColor Cyan
Push-Location $projectRoot
try {
    & pixi run build-release
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
    & pixi run package
    if ($LASTEXITCODE -ne 0) { throw 'Package failed' }
} finally {
    Pop-Location
}

# Step 4 - commit specific files only (avoid git add -A sweeping in build artifacts)
Write-Host 'Committing version + changelog...' -ForegroundColor Cyan
git add $manifestPath $cmakePath $installCmdPath $changelogPath $pixiPath $constantsPath
git commit -m "Release v$Version"
if ($LASTEXITCODE -ne 0) { throw 'Commit failed' }

# Step 5 - tag + push
Write-Host "Creating tag $tag..." -ForegroundColor Cyan
git tag -a $tag -m "Release $tag"
if ($LASTEXITCODE -ne 0) { throw "Could not create tag $tag" }
git push origin main
if ($LASTEXITCODE -ne 0) { throw 'Push of main failed - the tag exists locally; re-push once the remote accepts it.' }
git push origin $tag
if ($LASTEXITCODE -ne 0) { throw "Push of tag $tag failed - CI will not build until it lands." }

Write-Host ''
Write-Host "Release $tag pushed - CI will build and publish artifacts." -ForegroundColor Green
