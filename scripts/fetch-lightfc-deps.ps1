# Download LightFC checkpoint (Google Drive) and export ONNX for upPlayer.
# Requires: Python 3.10+, torch, gdown. Reuses third_party/onnxruntime from fetch-yolo-deps.ps1.
param(
    [string]$Python = "",
    [string]$CkptZip = "",
    [switch]$SkipDownload
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$LightDir = Join-Path $Root "third_party\lightfc"
$SrcDir = Join-Path $Root "third_party\LightFC-src"
New-Item -ItemType Directory -Force -Path $LightDir | Out-Null

function Get-Python {
    param([string]$Hint)
    if ($Hint -and (Test-Path $Hint)) { return $Hint }
    $candidates = @(
        "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
        "$env:LOCALAPPDATA\Programs\Python\Python311\python.exe",
        (Get-Command python.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
    )
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c) -and ($c -notmatch 'WindowsApps')) { return $c }
    }
    throw "Python not found. Install Python 3.12+ or pass -Python path."
}

function Download-File([string]$Url, [string]$Dest) {
    $mirrors = @(
        ("https://ghfast.top/" + $Url),
        $Url
    )
    foreach ($u in $mirrors) {
        Write-Host "Trying $u"
        Remove-Item $Dest -Force -ErrorAction SilentlyContinue
        & curl.exe -L --http1.1 --ssl-no-revoke --noproxy "*" --connect-timeout 30 --retry 2 -o $Dest $u
        if ((Test-Path $Dest) -and ((Get-Item $Dest).Length -gt 100KB)) { return }
    }
    throw "Download failed: $Url"
}

# 1) Source tree
if (-not (Test-Path (Join-Path $SrcDir "lib\models\tracker_model.py"))) {
    $zip = Join-Path $env:TEMP "LightFC-main.zip"
    Download-File "https://github.com/LiYunfengLYF/LightFC/archive/refs/heads/main.zip" $zip
    $stage = Join-Path $env:TEMP "LightFC-extract"
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    Expand-Archive $zip $stage -Force
    $inner = Get-ChildItem $stage -Directory | Select-Object -First 1
    Remove-Item $SrcDir -Recurse -Force -ErrorAction SilentlyContinue
    Move-Item $inner.FullName $SrcDir
}

$py = Get-Python -Hint $Python
Write-Host "Using $py"
& $py -m pip install -U torch torchvision --index-url https://download.pytorch.org/whl/cpu --quiet
& $py -m pip install -U pyyaml easydict opencv-python-headless onnx onnxruntime onnxsim gdown --quiet

# 2) Checkpoint (Google Drive id from LightFC README)
$ckptCandidates = @(
    (Join-Path $LightDir "lightfc_ep0400.pth.tar"),
    (Join-Path $SrcDir "outputs\checkpoints\train\lightfc\mobilnetv2_p_pwcorr_se_scf_sc_iab_sc_adj_concat_repn33_se_conv33_center_wiou\lightfc_ep0400.pth.tar")
)
$ckpt = $ckptCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $ckpt -and -not $SkipDownload) {
    $zipOut = if ($CkptZip) { $CkptZip } else { Join-Path $LightDir "outputs.zip" }
    if (-not (Test-Path $zipOut) -or ((Get-Item $zipOut).Length -lt 1MB)) {
        Write-Host "Downloading LightFC checkpoint zip via gdown (needs Google Drive access)..."
        & $py -m gdown "https://drive.google.com/uc?id=1ns7NQJCt078547X483skqjX1qM1rBqLP" -O $zipOut
    }
    if ((Test-Path $zipOut) -and ((Get-Item $zipOut).Length -gt 1MB)) {
        $stage = Join-Path $env:TEMP "lightfc-ckpt-extract"
        Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
        Expand-Archive $zipOut $stage -Force
        $found = Get-ChildItem $stage -Recurse -Filter "lightfc_ep*.pth.tar" | Select-Object -First 1
        if ($found) {
            Copy-Item $found.FullName (Join-Path $LightDir $found.Name) -Force
            $ckpt = Join-Path $LightDir $found.Name
        }
    }
}

$relZ = Join-Path $LightDir "lightfc_z.onnx"
$relX = Join-Path $LightDir "lightfc_x.onnx"
if ((Test-Path $relZ) -and (Test-Path $relX) -and ((Get-Item $relZ).Length -gt 100KB)) {
    Write-Host "ONNX already present: $relZ / $relX"
    Write-Host "Reconfigure/rebuild upPlayer after this script."
    exit 0
}

# Try GitHub release assets (if published)
$gh = Get-Command gh -ErrorAction SilentlyContinue
if ($gh -and -not ((Test-Path $relZ) -and (Test-Path $relX))) {
    Write-Host "Trying gh release download lightfc*.onnx..."
    & gh release download --repo moneypi/upPlayer --pattern "lightfc*.onnx" --dir $LightDir --clobber 2>$null
    if ((Test-Path $relZ) -and (Test-Path $relX)) {
        Write-Host "Got ONNX from GitHub release"
        Write-Host "Reconfigure/rebuild upPlayer after this script."
        exit 0
    }
}

if (-not $ckpt) {
    throw @"
LightFC checkpoint / ONNX not found.
Options:
  1) Place lightfc_z.onnx + lightfc_x.onnx in third_party\lightfc\
  2) Place lightfc_ep0400.pth.tar there (from LightFC Google Drive) and re-run
  3) Run with Google Drive access so gdown can fetch the official zip
"@
}

# 3) Export ONNX
Write-Host "Exporting ONNX from $ckpt"
& $py (Join-Path $Root "scripts\export-lightfc-onnx.py") --src $SrcDir --ckpt $ckpt --out $LightDir
if ($LASTEXITCODE -ne 0) { throw "export failed" }

Write-Host "LightFC ONNX: $LightDir\lightfc_z.onnx + lightfc_x.onnx"
Write-Host "Reconfigure/rebuild upPlayer after this script."
