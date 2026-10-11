# Push tag and publish GitHub release with upPlayer-Setup-x64.exe.
# Requires: gh auth login   OR   $env:GITHUB_TOKEN / $env:GH_TOKEN
param(
    [string]$Version = "0.2.0",
    [string]$SetupPath = "",
    [switch]$SkipPush
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Tag = "v$Version"
$OutDir = Join-Path $Root "build\dist"
if (-not $SetupPath) {
    $SetupPath = Join-Path $OutDir "upPlayer-Setup-x64.exe"
}
if (-not (Test-Path $SetupPath)) {
    throw "Missing setup: $SetupPath. Run scripts\package-setup.ps1 -AppVersion $Version first."
}

$versioned = Join-Path $OutDir "upPlayer-Setup-x64-$Tag.exe"
Copy-Item $SetupPath $versioned -Force

if (-not $SkipPush) {
    Write-Host "Pushing master and $Tag..."
    git -C $Root push origin HEAD:master
    git -C $Root push origin $Tag
}

$notes = @"
## upPlayer $Tag

Windows setup installer (NSIS / Nullsoft), installs to ``C:\Program Files\upPlayer``.

### Changes
- NSIS setup packaging (PotPlayer-style Program Files install)
- Speed: Ctrl+Up / Ctrl+Down (replaces [ / ])
- MinGW build link fixes (imm32, -municode)

### Install
Run ``upPlayer-Setup-x64-$Tag.exe`` (admin / UAC).
"@

$gh = Get-Command gh -ErrorAction SilentlyContinue
if ($gh) {
    Write-Host "Creating GitHub release with gh..."
    & gh release create $Tag $versioned `
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

$uploadUrl = $release.upload_url -replace '\{\?name,label\}', "?name=upPlayer-Setup-x64-$Tag.exe"
$fileBytes = [IO.File]::ReadAllBytes($versioned)
Invoke-RestMethod -Method Post `
    -Uri $uploadUrl `
    -Headers $headers `
    -ContentType "application/octet-stream" `
    -Body $fileBytes | Out-Null

Write-Host "Published: $($release.html_url)"
