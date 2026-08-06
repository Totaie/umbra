<#
.SYNOPSIS
    Downloads the Umbra host installer that the Umbra bundle chains.

.DESCRIPTION
    The installer offers to install a host alongside the client so one machine can
    both stream to other PCs and be streamed from. Rather than pinning Bundle.wxs to
    a DownloadUrl with a baked-in size and SHA512 (which would need editing on every
    host release), the payload is fetched here at build time and referenced as a
    local file.

    Umbra's host is a fork of Vibepollo, which is itself a fork of Apollo, which is a
    fork of Sunshine. All are GPL-3.0. Until Totaie/umbra-host produces its own signed
    installer, this pulls the upstream Vibepollo release.

.NOTES
    GPLv3: redistributing this binary in the Umbra installer obliges us to offer the
    corresponding source. Totaie/umbra-host is the public fork that satisfies that.
#>
[CmdletBinding()]
param(
    # Repo to pull the host installer release from.
    [string] $Repo = 'Nonary/vibepollo',

    # Release tag to fetch. 'latest' resolves the newest published release.
    [string] $Tag = 'latest',

    # Where the build expects to find the payload. Must match the ExePackagePayload
    # SourceFile in wix\UmbraSetup\Bundle.wxs. Defaults to build\host\ in the repo.
    [string] $OutFile
)

$ErrorActionPreference = 'Stop'

# NB: $PSScriptRoot isn't populated when param() defaults are evaluated, so this
# has to be resolved here rather than inline above.
if (-not $OutFile) {
    $OutFile = Join-Path $PSScriptRoot '..\build\host\UmbraHostSetup.exe'
}

$apiUrl = if ($Tag -eq 'latest') {
    "https://api.github.com/repos/$Repo/releases/latest"
} else {
    "https://api.github.com/repos/$Repo/releases/tags/$Tag"
}

Write-Host "Looking up host release from $Repo ($Tag)..." -ForegroundColor Cyan
$headers = @{ 'User-Agent' = 'umbra-build' }
if ($env:GITHUB_TOKEN) {
    # Avoids the very low unauthenticated rate limit on CI
    $headers['Authorization'] = "Bearer $env:GITHUB_TOKEN"
}

$release = Invoke-RestMethod -Uri $apiUrl -Headers $headers

$asset = $release.assets | Where-Object { $_.name -like '*Setup*.exe' } | Select-Object -First 1
if (-not $asset) {
    throw "No installer asset (*Setup*.exe) found in $Repo release '$($release.tag_name)'."
}

$outDir = Split-Path -Parent $OutFile
if (-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}

$sizeMb = [math]::Round($asset.size / 1MB, 1)
Write-Host "Downloading $($asset.name) ($sizeMb MB) from release $($release.tag_name)..." -ForegroundColor Cyan
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $OutFile -Headers $headers

$actual = (Get-Item $OutFile).Length
if ($actual -ne $asset.size) {
    Remove-Item $OutFile -Force
    throw "Download is $actual bytes but the release lists $($asset.size). Refusing to ship a truncated installer."
}

Write-Host "Host installer ready:" -ForegroundColor Green
Write-Host "  $OutFile"
Write-Host "  version $($release.tag_name), sha256 $((Get-FileHash $OutFile -Algorithm SHA256).Hash)"
