<#
.SYNOPSIS
    Builds Umbra and publishes the installer to GitHub Releases.

.DESCRIPTION
    Umbra updates itself from GitHub Releases: AutoUpdateChecker reads
    /releases/latest and looks for an installer asset whose name contains the
    running machine's architecture. This script produces exactly that.

    The version in app/version.txt is the source of truth. The tag is v<version>,
    and the asset is UmbraSetup-<arch>-<version>.exe so one release can carry both
    x64 and ARM64 builds.

.EXAMPLE
    .\scripts\publish-release.ps1
    Build and publish a release for the version in app/version.txt.

.EXAMPLE
    .\scripts\publish-release.ps1 -Bump patch
    Bump the patch version first, commit it, then build and publish.

.NOTES
    Requires the GitHub CLI (gh) to be installed and authenticated.

    These builds are unsigned, so Windows SmartScreen will warn on first run.
    Signing needs a code signing certificate; see SIGNTOOL_PARAMS in
    scripts/build-arch.bat for where it plugs in.
#>
[CmdletBinding()]
param(
    # Bump app/version.txt before building: major, minor or patch.
    [ValidateSet('major', 'minor', 'patch')]
    [string] $Bump,

    # Publish as a prerelease. AutoUpdateChecker uses /releases/latest, which skips
    # prereleases, so this is how to stage a build without offering it to everyone.
    [switch] $PreRelease,

    # Build and stage the release without creating it on GitHub.
    [switch] $DryRun,

    # Skip the host installer in the bundle (client-only build).
    [switch] $NoHost
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$versionFile = Join-Path $root 'app\version.txt'
$arch = 'x64'

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "The GitHub CLI (gh) is required. Install it from https://cli.github.com and run 'gh auth login'."
}

# --- version -------------------------------------------------------------------
$version = (Get-Content $versionFile -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') {
    throw "app/version.txt should contain a three part version like 1.2.3, found '$version'."
}

if ($Bump) {
    $parts = $version.Split('.') | ForEach-Object { [int] $_ }
    switch ($Bump) {
        'major' { $parts = @($parts[0] + 1, 0, 0) }
        'minor' { $parts = @($parts[0], $parts[1] + 1, 0) }
        'patch' { $parts = @($parts[0], $parts[1], $parts[2] + 1) }
    }
    $version = $parts -join '.'
    Set-Content -Path $versionFile -Value $version -Encoding utf8 -NoNewline
    Write-Host "Version bumped to $version" -ForegroundColor Cyan
}

$tag = "v$version"

# Refuse to publish a tag that already exists rather than silently doing nothing.
$existing = & gh release view $tag --json tagName 2>$null
if ($LASTEXITCODE -eq 0 -and -not $DryRun) {
    throw "Release $tag already exists. Bump the version (-Bump patch) or delete the existing release."
}

# --- build ---------------------------------------------------------------------
Write-Host "Building Umbra $version..." -ForegroundColor Cyan
$installerArgs = @('release')
if ($NoHost) { $installerArgs += '--no-host' }

& cmd /c (Join-Path $root 'scripts\umbra-installer.bat') @installerArgs
if ($LASTEXITCODE -ne 0) {
    throw "Build failed."
}

$builtSetup = Join-Path $root "build\installer-$arch-release\UmbraSetup.exe"
if (-not (Test-Path $builtSetup)) {
    throw "Expected installer at $builtSetup but it wasn't produced."
}

# The update checker matches assets on architecture, so it has to be in the name.
$assetName = "UmbraSetup-$arch-$version.exe"
$assetPath = Join-Path $root "build\installer-$arch-release\$assetName"
Copy-Item $builtSetup $assetPath -Force

$sizeMb = [math]::Round((Get-Item $assetPath).Length / 1MB, 1)
$sha = (Get-FileHash $assetPath -Algorithm SHA256).Hash
Write-Host "Built $assetName ($sizeMb MB)" -ForegroundColor Green
Write-Host "  sha256 $sha"

if ($DryRun) {
    Write-Host "`nDry run: not publishing. Asset staged at" -ForegroundColor Yellow
    Write-Host "  $assetPath"
    exit 0
}

# --- publish -------------------------------------------------------------------
$notes = @"
Umbra $version

Installer: ``$assetName`` ($sizeMb MB)
SHA256: ``$sha``

Existing installs will offer this update automatically. These builds are unsigned,
so Windows SmartScreen will warn the first time you run the installer.
"@

$ghArgs = @('release', 'create', $tag, $assetPath,
            '--title', "Umbra $version",
            '--notes', $notes)
if ($PreRelease) { $ghArgs += '--prerelease' }

Write-Host "Publishing $tag..." -ForegroundColor Cyan
& gh @ghArgs
if ($LASTEXITCODE -ne 0) {
    throw "gh release create failed."
}

Write-Host "`nPublished $tag" -ForegroundColor Green
if ($PreRelease) {
    Write-Host "Marked as a prerelease, so it will NOT be offered as an update until promoted." -ForegroundColor Yellow
}
Write-Host "Remember to commit and push app/version.txt if you bumped it."
