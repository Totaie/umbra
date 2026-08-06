<#
.SYNOPSIS
    Builds the Umbra host and publishes the installer to GitHub Releases.

.DESCRIPTION
    The host already knows how to update itself: src/update.cpp polls the GitHub
    releases of SUNSHINE_REPO_OWNER/SUNSHINE_REPO_NAME, which cmake/prep/build_version.cmake
    points at Totaie/umbra-host for this fork. This script produces the releases it
    reads, on the same cadence as the client's scripts/publish-release.ps1.

    Version comes from the CMake project version, and the tag is v<version>.

.EXAMPLE
    .\scripts\publish-release.ps1
    Build and publish a release.

.EXAMPLE
    .\scripts\publish-release.ps1 -DryRun
    Build and package without touching GitHub.

.NOTES
    Must be run from Windows PowerShell, not the MSYS2 shell: it shells into MSYS2
    for the build itself. Requires the GitHub CLI (gh), authenticated.

    Builds are unsigned, so SmartScreen will warn on first run, and the host
    installs a service and a virtual display driver, which makes that warning more
    alarming than it is for the client.
#>
[CmdletBinding()]
param(
    # Publish as a prerelease so it isn't offered as an update yet.
    [switch] $PreRelease,

    # Build and package without creating the release.
    [switch] $DryRun,

    # Where MSYS2 is installed.
    [string] $Msys2Root = 'C:\msys64'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$arch = 'x64'

if (-not $DryRun -and -not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "The GitHub CLI (gh) is required. Install it from https://cli.github.com and run 'gh auth login'."
}

$bash = Join-Path $Msys2Root 'usr\bin\bash.exe'
if (-not (Test-Path $bash)) {
    throw "MSYS2 not found at $Msys2Root. Install it, or pass -Msys2Root."
}

# --- build ---------------------------------------------------------------------
# PROCESSOR_ARCHITECTURE has to reach CMake or the FFmpeg dependency download builds
# the wrong asset name and 404s; see scripts/umbra_windows_build.sh for the details.
$unixRoot = ($root -replace '\\', '/') -replace '^([A-Za-z]):', { "/$($_.Groups[1].Value.ToLower())" }

Write-Host "Building the Umbra host (this takes a while)..." -ForegroundColor Cyan
$buildCmd = @"
export MSYSTEM=UCRT64
source /etc/profile
export PROCESSOR_ARCHITECTURE=AMD64
cd '$unixRoot' || exit 1
cmake -B build -G Ninja -S . -DSUNSHINE_REPO_OWNER=Totaie -DSUNSHINE_REPO_NAME=umbra-host || exit 1
ninja -C build || exit 1
cpack -G NSIS --config ./build/CPackConfig.cmake || exit 1
"@

& $bash -lc $buildCmd
if ($LASTEXITCODE -ne 0) {
    throw "Build failed."
}

# CPack names the installer from the project version; find whatever it produced.
$package = Get-ChildItem (Join-Path $root 'build') -Filter '*.exe' |
           Where-Object { $_.Name -match 'Setup|Installer|sunshine|umbra' -and $_.Name -notmatch 'tools' } |
           Sort-Object LastWriteTime | Select-Object -Last 1

if (-not $package) {
    throw "cpack completed but no installer was found in build\. Check the cpack output above."
}

# Read the version CMake configured, so the tag matches what the running host reports
# and the update check compares like for like.
$versionLine = Select-String -Path (Join-Path $root 'build\CPackConfig.cmake') -Pattern 'CPACK_PACKAGE_VERSION "([^"]+)"' | Select-Object -First 1
if (-not $versionLine) {
    throw "Could not read CPACK_PACKAGE_VERSION from build\CPackConfig.cmake."
}
$version = $versionLine.Matches[0].Groups[1].Value
$tag = "v$version"

$assetName = "UmbraHostSetup-$arch-$version.exe"
$assetPath = Join-Path $root "build\$assetName"
Copy-Item $package.FullName $assetPath -Force

$sizeMb = [math]::Round((Get-Item $assetPath).Length / 1MB, 1)
$sha = (Get-FileHash $assetPath -Algorithm SHA256).Hash
Write-Host "Built $assetName ($sizeMb MB)" -ForegroundColor Green
Write-Host "  sha256 $sha"

if ($DryRun) {
    Write-Host "`nDry run: not publishing. Asset staged at" -ForegroundColor Yellow
    Write-Host "  $assetPath"
    exit 0
}

$existing = & gh release view $tag --json tagName 2>$null
if ($LASTEXITCODE -eq 0) {
    throw "Release $tag already exists. Bump the project version or delete the existing release."
}

# --- publish -------------------------------------------------------------------
$notes = @"
Umbra Host $version

Installer: ``$assetName`` ($sizeMb MB)
SHA256: ``$sha``

Existing hosts will pick this up on their next update check. Builds are unsigned,
so SmartScreen will warn on first run.
"@

$ghArgs = @('release', 'create', $tag, $assetPath,
            '--title', "Umbra Host $version",
            '--notes', $notes)
if ($PreRelease) { $ghArgs += '--prerelease' }

Write-Host "Publishing $tag..." -ForegroundColor Cyan
& gh @ghArgs
if ($LASTEXITCODE -ne 0) {
    throw "gh release create failed."
}

Write-Host "`nPublished $tag" -ForegroundColor Green
