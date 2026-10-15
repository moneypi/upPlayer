# Push tag and publish GitHub release with AI and no-AI setup installers.
# Requires: gh auth login   OR   $env:GITHUB_TOKEN / $env:GH_TOKEN
param(
    [string]$Version = "",
    [string]$AiSetupPath = "",
    [string]$NoAiSetupPath = "",
    [switch]$SkipPush
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

function Get-UpPlayerVersion {
    $header = Join-Path $Root "src\version.hpp"
    $text = Get-Content $header -Raw
    if ($text -match 'UPPLAYER_VERSION_STRING\s+"([^"]+)"') {
        return $Matches[1]
    }
    throw "Cannot read UPPLAYER_VERSION_STRING from src\version.hpp"
}

if (-not $Version) {
    $Version = Get-UpPlayerVersion
}
$Tag = "v$Version"
$OutDir = Join-Path $Root "build\dist"
$aiVersioned = Join-Path $OutDir "upPlayer-Setup-x64-$Version-AI.exe"
$noAiVersioned = Join-Path $OutDir "upPlayer-Setup-x64-$Version-noAI.exe"

if (-not $AiSetupPath) { $AiSetupPath = $aiVersioned }
if (-not $NoAiSetupPath) { $NoAiSetupPath = $noAiVersioned }

if (-not (Test-Path $AiSetupPath)) {
    throw "Missing AI setup: $AiSetupPath. Run scripts\package-both.ps1 first."
}
if (-not (Test-Path $NoAiSetupPath)) {
    throw "Missing no-AI setup: $NoAiSetupPath. Run scripts\package-both.ps1 first."
}
if ($AiSetupPath -ne $aiVersioned) {
    Copy-Item $AiSetupPath $aiVersioned -Force
}
if ($NoAiSetupPath -ne $noAiVersioned) {
    Copy-Item $NoAiSetupPath $noAiVersioned -Force
}

$changelog = Join-Path $Root "CHANGELOG.md"
$notesBody = ""
if (Test-Path $changelog) {
    $raw = Get-Content $changelog -Raw
    if ($raw -match "(?s)## $([regex]::Escape($Version))\r?\n(.*?)(?=\r?\n## |\z)") {
        $notesBody = $Matches[1].Trim()
    }
}
if (-not $notesBody) {
    $notesBody = "- See CHANGELOG.md"
}

$notes = @"
## upPlayer $Tag

Two Windows setup installers (choose folder; Finish / Start). Default: ``C:\Program Files\upPlayer``.

| Installer | Contents |
|-----------|----------|
| ``upPlayer-Setup-x64-$Version-AI.exe`` | Player + mpv + Whisper + YOLO26 + LightFC |
| ``upPlayer-Setup-x64-$Version-noAI.exe`` | Player + mpv only (smaller) |

### Changes
$notesBody

### Install
Run either setup (admin / UAC). The AI menu appears only in the AI build.
"@

if (-not $SkipPush) {
    Write-Host "Pushing master and $Tag..."
    git -C $Root push origin HEAD:master
    git -C $Root push origin $Tag
}

$assets = @($aiVersioned, $noAiVersioned)
$gh = Get-Command gh -ErrorAction SilentlyContinue
if ($gh) {
    Write-Host "Creating GitHub release with gh..."
    & gh release create $Tag @assets `
        --repo moneypi/upPlayer `
        --title "upPlayer $Tag" `
        --notes $notes `
        --latest
    if ($LASTEXITCODE -ne 0) { throw "gh release create failed ($LASTEXITCODE)" }
    Write-Host "Published: https://github.com/moneypi/upPlayer/releases/tag/$Tag"
    return
}

$token = $env:GITHUB_TOKEN
if (-not $token) { $token = $env:GH_TOKEN }
if (-not $token) {
    throw "No gh CLI and no GITHUB_TOKEN/GH_TOKEN. Run: gh auth login   OR set a PAT with repo scope."
}

Write-Host "Creating GitHub release via API..."
$releaseBody = @{
    tag_name         = $Tag
    name             = "upPlayer $Tag"
    body             = $notes
    draft            = $false
    prerelease       = $false
    make_latest      = "true"
} | ConvertTo-Json

$headers = @{
    Authorization          = "Bearer $token"
    Accept                 = "application/vnd.github+json"
    "X-GitHub-Api-Version" = "2022-11-28"
    "User-Agent"           = "upPlayer-publish-script"
}

$release = Invoke-RestMethod -Method Post `
    -Uri "https://api.github.com/repos/moneypi/upPlayer/releases" `
    -Headers $headers `
    -ContentType "application/json" `
    -Body $releaseBody

foreach ($asset in $assets) {
    $name = Split-Path $asset -Leaf
    $uploadUrl = $release.upload_url -replace '\{\?name,label\}', "?name=$name"
    $fileBytes = [IO.File]::ReadAllBytes($asset)
    Invoke-RestMethod -Method Post `
        -Uri $uploadUrl `
        -Headers $headers `
        -ContentType "application/octet-stream" `
        -Body $fileBytes | Out-Null
}

Write-Host "Published: $($release.html_url)"
