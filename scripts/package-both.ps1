# Build both setup installers: with AI and without AI.
param(
    [string]$Configuration = "Release",
    [string]$OutDir = "",
    [string]$ReleaseTag = "latest",
    [string]$AppVersion = "",
    [switch]$SkipPackage
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$setup = Join-Path $PSScriptRoot "package-setup.ps1"

Write-Host "=== Building AI setup ==="
& $setup -Configuration $Configuration -OutDir $OutDir -ReleaseTag $ReleaseTag `
    -AppVersion $AppVersion -IncludeAi:$true -SetupSuffix "-AI" -SkipPackage:$SkipPackage

Write-Host "=== Building no-AI setup ==="
& $setup -Configuration $Configuration -OutDir $OutDir -ReleaseTag $ReleaseTag `
    -AppVersion $AppVersion -IncludeAi:$false -SetupSuffix "-noAI" -SkipPackage:$SkipPackage

if (-not $OutDir) {
    $OutDir = Join-Path $Root "build\dist"
}
if (-not $AppVersion) {
    $header = Get-Content (Join-Path $Root "src\version.hpp") -Raw
    if ($header -match 'UPPLAYER_VERSION_STRING\s+"([^"]+)"') {
        $AppVersion = $Matches[1]
    }
}

$ai = Join-Path $OutDir "upPlayer-Setup-x64-$AppVersion-AI.exe"
$noAi = Join-Path $OutDir "upPlayer-Setup-x64-$AppVersion-noAI.exe"
Write-Host "AI:   $ai ($((Get-Item $ai).Length) bytes)"
Write-Host "noAI: $noAi ($((Get-Item $noAi).Length) bytes)"
