# Build lite/full Windows zip packages for upPlayer.
# Full package downloads the official Windows build from
# https://github.com/mpv-player/mpv/releases
param(
    [string]$Configuration = "Release",
    [string]$OutDir = "",
    # "latest" follows GitHub latest release; or pass a tag like "v0.41.0" / "git-release".
    [string]$ReleaseTag = "latest"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) {
    $OutDir = Join-Path $Root "build\dist"
}

$Exe = Join-Path $Root "build\$Configuration\upPlayer.exe"
if (-not (Test-Path $Exe)) {
    throw "Missing $Exe. Build the project first."
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Get-Release([string]$tag) {
    $url = if ($tag -eq "latest") {
        "https://api.github.com/repos/mpv-player/mpv/releases/latest"
    } else {
        "https://api.github.com/repos/mpv-player/mpv/releases/tags/$tag"
    }
    $json = curl.exe -sS --http1.1 --ssl-no-revoke -H "Accept: application/vnd.github+json" $url
    $release = $json | ConvertFrom-Json
    if (-not $release.tag_name) {
        throw "Failed to query mpv release ($tag): $json"
    }
    return $release
}

function Find-WindowsAsset($release) {
    $asset = $release.assets | Where-Object { $_.name -match 'x86_64-w64-mingw32.*\.zip$' } | Select-Object -First 1
    if (-not $asset) {
        $asset = $release.assets | Where-Object {
            $_.name -match 'x86_64-pc-windows-msvc\.zip$' -and $_.name -notmatch 'pdb'
        } | Select-Object -First 1
    }
    return $asset
}

function Download-File([string]$Url, [string]$Destination) {
    $urls = @(
        $Url,
        ("https://ghfast.top/" + $Url)
    )
    foreach ($candidate in $urls) {
        Write-Host "Trying $candidate"
        Remove-Item $Destination -Force -ErrorAction SilentlyContinue
        & curl.exe -L --http1.1 --ssl-no-revoke --connect-timeout 30 --retry 2 --retry-delay 2 `
            -o $Destination $candidate
        if ((Test-Path $Destination) -and ((Get-Item $Destination).Length -gt 1MB)) {
            return
        }
    }
    throw "Download failed for $Url"
}

Write-Host "Querying official mpv-player/mpv release ($ReleaseTag)..."
$release = Get-Release $ReleaseTag
$asset = Find-WindowsAsset $release

if (-not $asset) {
    Write-Host "No Windows x64 asset on $($release.tag_name); scanning recent releases..."
    $listJson = curl.exe -sS --http1.1 --ssl-no-revoke -H "Accept: application/vnd.github+json" `
        "https://api.github.com/repos/mpv-player/mpv/releases?per_page=10"
    $list = $listJson | ConvertFrom-Json
    foreach ($candidate in $list) {
        $asset = Find-WindowsAsset $candidate
        if ($asset) {
            $release = $candidate
            break
        }
    }
}
if (-not $asset) {
    throw "No official Windows x86_64 zip found on mpv-player/mpv releases."
}

$Archive = Join-Path $OutDir $asset.name
if ((Test-Path $Archive) -and ((Get-Item $Archive).Length -eq $asset.size)) {
    Write-Host "Using cached $($asset.name)"
} else {
    Write-Host "Downloading $($asset.name) ($([math]::Round($asset.size / 1MB, 1)) MiB) from $($release.tag_name)..."
    Download-File $asset.browser_download_url $Archive
}

$Extract = Join-Path $OutDir "mpv-extracted"
Remove-Item $Extract -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $Extract | Out-Null
Expand-Archive -Path $Archive -DestinationPath $Extract -Force

# Official release assets are sometimes a zip-of-zip (one nested .zip entry).
$nested = Get-ChildItem $Extract -Recurse -Filter "*.zip" | Select-Object -First 1
if ($nested) {
    Write-Host "Expanding nested $($nested.Name)..."
    $NestedDir = Join-Path $Extract "nested"
    New-Item -ItemType Directory -Force -Path $NestedDir | Out-Null
    Expand-Archive -Path $nested.FullName -DestinationPath $NestedDir -Force
    $Extract = $NestedDir
}

$mpvExe = Get-ChildItem $Extract -Recurse -Filter "mpv.exe" | Select-Object -First 1
if (-not $mpvExe) {
    throw "mpv.exe not found after extracting $Archive"
}
$mpvRoot = $mpvExe.Directory.FullName

$LiteDir = Join-Path $OutDir "lite"
$FullDir = Join-Path $OutDir "full"
Remove-Item $LiteDir, $FullDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $LiteDir, $FullDir | Out-Null

Copy-Item $Exe $LiteDir
Copy-Item $Exe $FullDir
Copy-Item $mpvExe.FullName $FullDir

Get-ChildItem $mpvRoot -File -Filter "*.dll" | ForEach-Object {
    Copy-Item $_.FullName $FullDir -Force
}

$fonts = Join-Path $mpvRoot "mpv\fonts.conf"
if (Test-Path $fonts) {
    New-Item -ItemType Directory -Force -Path (Join-Path $FullDir "mpv") | Out-Null
    Copy-Item $fonts (Join-Path $FullDir "mpv")
}

$LiteReadme = @"
upPlayer (lite)

Requires mpv.exe on PATH, or place libmpv-2.dll next to upPlayer.exe.

Run: upPlayer.exe
"@
$FullReadme = @"
upPlayer (full)

Bundled official mpv Windows build:
  source: https://github.com/mpv-player/mpv/releases
  tag:    $($release.tag_name)
  asset:  $($asset.name)

Includes mpv.exe and its shipped runtime DLLs. No separate mpv install is required.

Run: upPlayer.exe
"@
[IO.File]::WriteAllText((Join-Path $LiteDir "README.txt"), $LiteReadme, (New-Object Text.UTF8Encoding $false))
[IO.File]::WriteAllText((Join-Path $FullDir "README.txt"), $FullReadme, (New-Object Text.UTF8Encoding $false))

$LiteZip = Join-Path $OutDir "upPlayer-windows-x64-lite.zip"
$FullZip = Join-Path $OutDir "upPlayer-windows-x64-full.zip"
Remove-Item $LiteZip, $FullZip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $LiteDir '*') -DestinationPath $LiteZip -Force
Compress-Archive -Path (Join-Path $FullDir '*') -DestinationPath $FullZip -Force

Write-Host "lite: $LiteZip ($((Get-Item $LiteZip).Length) bytes)"
Write-Host "full: $FullZip ($((Get-Item $FullZip).Length) bytes)"
Write-Host "mpv:  $($release.tag_name) / $($asset.name)"
