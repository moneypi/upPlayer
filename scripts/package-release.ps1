# Build a Windows zip package for upPlayer.
# Downloads the official Windows build from https://github.com/mpv-player/mpv/releases
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

$ExeCandidates = @(
    (Join-Path $Root "out\$Configuration\upPlayer.exe"),
    (Join-Path $Root "build\$Configuration\upPlayer.exe"),
    (Join-Path $Root "out\upPlayer.exe")
)
$Exe = $ExeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Exe) {
    throw "Missing upPlayer.exe. Build the project first (looked in build\$Configuration, out\$Configuration, out)."
}
Write-Host "Using $Exe"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Get-Release([string]$tag) {
    $url = if ($tag -eq "latest") {
        "https://api.github.com/repos/mpv-player/mpv/releases/latest"
    } else {
        "https://api.github.com/repos/mpv-player/mpv/releases/tags/$tag"
    }
    $json = Invoke-GitHubApi $url
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
        & curl.exe -L --http1.1 --ssl-no-revoke --noproxy "*" --connect-timeout 30 --retry 2 --retry-delay 2 `
            --resolve "github.com:443:140.82.113.3" `
            -o $Destination $candidate
        if ((Test-Path $Destination) -and ((Get-Item $Destination).Length -gt 1MB)) {
            return
        }
    }
    throw "Download failed for $Url"
}

function Invoke-GitHubApi([string]$Url) {
    $json = curl.exe -sS --http1.1 --ssl-no-revoke --noproxy "*" `
        --resolve "github.com:443:140.82.113.3" `
        -H "Accept: application/vnd.github+json" $Url
    if (-not $json) {
        throw "Empty response from $Url"
    }
    return $json
}

Write-Host "Querying official mpv-player/mpv release ($ReleaseTag)..."
$release = Get-Release $ReleaseTag
$asset = Find-WindowsAsset $release

if (-not $asset) {
    Write-Host "No Windows x64 asset on $($release.tag_name); scanning recent releases..."
    $listJson = Invoke-GitHubApi "https://api.github.com/repos/mpv-player/mpv/releases?per_page=10"
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

$PkgDir = Join-Path $OutDir "package"
Remove-Item $PkgDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $PkgDir | Out-Null

Copy-Item $Exe $PkgDir
Copy-Item $mpvExe.FullName $PkgDir

Get-ChildItem $mpvRoot -File -Filter "*.dll" | ForEach-Object {
    Copy-Item $_.FullName $PkgDir -Force
}

foreach ($doc in @("LICENSE", "NOTICE", "CHANGELOG.md", "README.md")) {
    $src = Join-Path $Root $doc
    if (Test-Path $src) {
        Copy-Item $src $PkgDir -Force
    }
}

foreach ($script in @("register-assoc.ps1", "install-payload.ps1")) {
    $src = Join-Path $PSScriptRoot $script
    if (Test-Path $src) {
        Copy-Item $src $PkgDir -Force
    }
}

# PotPlayer Whisper is proprietary — only include a local copy if the developer
# already placed it next to the built exe or under third_party (gitignored).
# Do not publish those binaries unless you have redistrib rights; see NOTICE.
$whisperCandidates = @(
    (Join-Path (Split-Path $Exe -Parent) "potplayer-whisper"),
    (Join-Path $Root "third_party\potplayer-whisper")
)
$whisperSrc = $whisperCandidates | Where-Object {
    (Test-Path (Join-Path $_ "Vulkan\main64.exe")) -or (Test-Path (Join-Path $_ "CPU\main64.exe"))
} | Select-Object -First 1
if ($whisperSrc) {
    Write-Host "Including local PotPlayer Whisper from $whisperSrc"
    Copy-Item $whisperSrc (Join-Path $PkgDir "potplayer-whisper") -Recurse -Force
} else {
    Write-Host "PotPlayer Whisper not found next to build; captions need a local Module\Whisper copy (see README)."
}

# YOLO26 (optional): onnx next to build + ONNX Runtime DLLs
$yoloOnnxCandidates = @(
    (Join-Path (Split-Path $Exe -Parent) "yolo26\yolo26n.onnx"),
    (Join-Path $Root "third_party\yolo26\yolo26n.onnx")
)
$yoloOnnx = $yoloOnnxCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($yoloOnnx) {
    $yoloDest = Join-Path $PkgDir "yolo26"
    New-Item -ItemType Directory -Force -Path $yoloDest | Out-Null
    Copy-Item $yoloOnnx (Join-Path $yoloDest "yolo26n.onnx") -Force
    Write-Host "Including YOLO26 model $yoloOnnx"
}
$ortDllCandidates = @(
    (Join-Path (Split-Path $Exe -Parent) "onnxruntime.dll"),
    (Join-Path $Root "third_party\onnxruntime\lib\onnxruntime.dll")
)
$ortDll = $ortDllCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($ortDll) {
    Copy-Item $ortDll $PkgDir -Force
    $shared = Join-Path (Split-Path $ortDll -Parent) "onnxruntime_providers_shared.dll"
    if (Test-Path $shared) { Copy-Item $shared $PkgDir -Force }
    Write-Host "Including ONNX Runtime from $(Split-Path $ortDll -Parent)"
}

$fonts = Join-Path $mpvRoot "mpv\fonts.conf"
if (Test-Path $fonts) {
    New-Item -ItemType Directory -Force -Path (Join-Path $PkgDir "mpv") | Out-Null
    Copy-Item $fonts (Join-Path $PkgDir "mpv")
}

$Readme = @"
upPlayer

Bundled official mpv Windows build:
  source: https://github.com/mpv-player/mpv/releases
  tag:    $($release.tag_name)
  asset:  $($asset.name)

Includes mpv.exe and its shipped runtime DLLs. No separate mpv install is required.
Clip export (z/x/c) uses the bundled mpv.exe.

Run: upPlayer.exe
"@
[IO.File]::WriteAllText((Join-Path $PkgDir "README.txt"), $Readme, (New-Object Text.UTF8Encoding $false))

$Zip = Join-Path $OutDir "upPlayer-windows-x64.zip"
Remove-Item $Zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $PkgDir '*') -DestinationPath $Zip -Force

Write-Host "package: $Zip ($((Get-Item $Zip).Length) bytes)"
Write-Host "mpv:     $($release.tag_name) / $($asset.name)"
