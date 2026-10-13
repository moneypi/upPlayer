# Download MIT-licensed whisper.cpp Windows CLI + ggml-base model.
# Prefers a Vulkan build (GPU) for live captions; falls back to official CPU zip.
param(
    [string]$ReleaseTag = "v1.8.5",
    # Vulkan community build (ggml-org does not ship Windows Vulkan zips yet).
    [string]$VulkanRepo = "jonasthilo/whisper.cpp",
    [string]$VulkanAsset = "whisper-vulkan-bin-x64.zip",
    [string]$CpuReleaseTag = "v1.8.6",
    [string]$CpuAsset = "whisper-bin-x64.zip",
    [switch]$CpuOnly,
    [switch]$SkipModel,
    [switch]$UseXunlei,
    [int]$XunleiTimeoutSec = 600
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$WhisperDir = Join-Path $Root "third_party\whisper-win"
$ModelDir = Join-Path $WhisperDir "models"
New-Item -ItemType Directory -Force -Path $WhisperDir, $ModelDir | Out-Null

function Find-ThunderExe {
    foreach ($c in @(
        "C:\Program Files\Thunder Network\Thunder\program\Thunder.exe",
        "C:\Program Files\Thunder Network\Thunder\Program\Thunder.exe"
    )) {
        if (Test-Path -LiteralPath $c) { return $c }
    }
    return $null
}

function Get-XunleiDownloadDirs {
    $dirs = @()
    foreach ($d in @("D:\迅雷下载", "C:\迅雷下载", (Join-Path $env:USERPROFILE "Downloads"))) {
        if ($d -and (Test-Path -LiteralPath $d)) { $dirs += $d }
    }
    return $dirs
}

function Download-WithXunlei([string]$Url, [string]$Dest, [string]$ExpectedName, [int]$MinBytes, [int]$TimeoutSec) {
    $thunder = Find-ThunderExe
    if (-not $thunder) { throw "迅雷 not found. Install Thunder or omit -UseXunlei." }
    Write-Host "Opening 迅雷 for: $Url"
    Start-Process -FilePath $thunder -ArgumentList $Url | Out-Null
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $lastLen = -1
    $stable = 0
    while ((Get-Date) -lt $deadline) {
        $hits = @()
        foreach ($dir in (Get-XunleiDownloadDirs)) {
            $hits += Get-ChildItem -LiteralPath $dir -File -EA SilentlyContinue |
                Where-Object { $_.Name -eq $ExpectedName -or $_.Name -like "$ExpectedName*" }
        }
        $done = $hits | Where-Object {
            $_.Name -eq $ExpectedName -and $_.Length -ge $MinBytes -and $_.Extension -notin @(".xltd", ".td")
        } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($done) {
            if ($done.Length -eq $lastLen) { $stable++ } else { $stable = 0; $lastLen = $done.Length }
            Write-Host ("Found {0} ({1:N0} bytes)" -f $done.FullName, $done.Length)
            if ($stable -ge 2) {
                Copy-Item -LiteralPath $done.FullName -Destination $Dest -Force
                return
            }
        } else {
            Write-Host "Waiting for 迅雷: $ExpectedName ..."
        }
        Start-Sleep -Seconds 5
    }
    throw "Timed out waiting for 迅雷 ($ExpectedName)"
}

function Download-File([string]$Url, [string]$Dest, [string]$ExpectedName, [int]$MinBytes = 100KB) {
    if ($UseXunlei) {
        Download-WithXunlei $Url $Dest $ExpectedName $MinBytes $XunleiTimeoutSec
        return
    }
    foreach ($u in @(("https://ghfast.top/" + $Url), $Url)) {
        Write-Host "Trying $u"
        Remove-Item $Dest -Force -EA SilentlyContinue
        & curl.exe -L --http1.1 --ssl-no-revoke --noproxy "*" --connect-timeout 30 --retry 2 -o $Dest $u
        if ((Test-Path $Dest) -and ((Get-Item $Dest).Length -gt $MinBytes)) { return }
    }
    Write-Host "curl failed; falling back to 迅雷..."
    Download-WithXunlei $Url $Dest $ExpectedName $MinBytes $XunleiTimeoutSec
}

function Install-CliFromZip([string]$ZipPath) {
    $stage = Join-Path $env:TEMP ("whisper-extract-" + [guid]::NewGuid().ToString("N"))
    Remove-Item $stage -Recurse -Force -EA SilentlyContinue
    Expand-Archive -LiteralPath $ZipPath -DestinationPath $stage -Force
    $exe = Get-ChildItem $stage -Recurse -Include whisper-cli.exe, main.exe, whisper.exe | Select-Object -First 1
    if (-not $exe) { throw "No whisper CLI in zip" }
    $srcDir = $exe.Directory.FullName
    Get-ChildItem $WhisperDir -File | Remove-Item -Force
    Get-ChildItem $srcDir -File | Copy-Item -Destination $WhisperDir -Force
    $hasVulkan = Test-Path (Join-Path $WhisperDir "ggml-vulkan.dll")
    Write-Host ("Installed {0} (Vulkan={1})" -f $exe.Name, $hasVulkan)
    Remove-Item $stage -Recurse -Force -EA SilentlyContinue
}

$haveVulkan = Test-Path (Join-Path $WhisperDir "ggml-vulkan.dll")
$haveCli = @(
    (Join-Path $WhisperDir "whisper-cli.exe"),
    (Join-Path $WhisperDir "main.exe")
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $CpuOnly -and -not $haveVulkan) {
    $zip = Join-Path $env:TEMP $VulkanAsset
    $url = "https://github.com/$VulkanRepo/releases/download/$ReleaseTag/$VulkanAsset"
    try {
        Download-File $url $zip $VulkanAsset 1MB
        Install-CliFromZip $zip
        $haveVulkan = Test-Path (Join-Path $WhisperDir "ggml-vulkan.dll")
        $haveCli = $true
    } catch {
        Write-Host "Vulkan download failed: $($_.Exception.Message)"
        Write-Host "Falling back to official CPU build..."
    }
}

if (-not $haveCli -or ($CpuOnly -and -not (Test-Path (Join-Path $WhisperDir "whisper-cli.exe")))) {
    $zip = Join-Path $env:TEMP "whisper-cpp-$CpuReleaseTag-$CpuAsset"
    $url = "https://github.com/ggml-org/whisper.cpp/releases/download/$CpuReleaseTag/$CpuAsset"
    Download-File $url $zip $CpuAsset 100KB
    Install-CliFromZip $zip
}

if (-not $SkipModel) {
    $model = Join-Path $ModelDir "ggml-base.bin"
    if (-not (Test-Path $model) -or ((Get-Item $model).Length -lt 1MB)) {
        $modelUrl = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin"
        Download-File $modelUrl $model "ggml-base.bin" 1MB
    }
    Write-Host "Model: $model"
}

$release = Join-Path $Root "out\Release"
if (Test-Path $release) {
    $dest = Join-Path $release "whisper"
    New-Item -ItemType Directory -Force -Path $dest, (Join-Path $dest "models") | Out-Null
    Get-ChildItem $WhisperDir -File | Copy-Item -Destination $dest -Force
    if (Test-Path $ModelDir) {
        Copy-Item (Join-Path $ModelDir "*") (Join-Path $dest "models") -Force
    }
    Write-Host "Staged: $dest (Vulkan=$(Test-Path (Join-Path $dest 'ggml-vulkan.dll')))"
}

Write-Host "Done. Prefer Vulkan GPU captions when ggml-vulkan.dll is present."
