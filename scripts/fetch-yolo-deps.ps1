# Download ONNX Runtime + export Ultralytics YOLO26n to ONNX (NMS-free).
param(
    [string]$OrtVersion = "1.22.0",
    [string]$Python = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$YoloDir = Join-Path $Root "third_party\yolo26"
$OrtDir = Join-Path $Root "third_party\onnxruntime"
New-Item -ItemType Directory -Force -Path $YoloDir | Out-Null

function Get-Python {
    param([string]$Hint)
    if ($Hint -and (Test-Path $Hint)) { return $Hint }
    $candidates = @(
        "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
        "$env:LOCALAPPDATA\Programs\Python\Python311\python.exe",
        (Get-Command python.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
    )
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
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
        if ((Test-Path $Dest) -and ((Get-Item $Dest).Length -gt 1MB)) {
            return
        }
    }
    throw "Download failed: $Url"
}

$pt = Join-Path $YoloDir "yolo26n.pt"
if (-not (Test-Path $pt) -or ((Get-Item $pt).Length -lt 1MB)) {
    Download-File "https://github.com/ultralytics/assets/releases/download/v8.4.0/yolo26n.pt" $pt
}

$onnx = Join-Path $YoloDir "yolo26n.onnx"
if (-not (Test-Path $onnx) -or ((Get-Item $onnx).Length -lt 1MB)) {
    $py = Get-Python -Hint $Python
    Write-Host "Exporting YOLO26n ONNX with $py"
    & $py -m pip install -U ultralytics onnx --quiet
    & $py -c "from ultralytics import YOLO; m=YOLO(r'$pt'); print(m.export(format='onnx', nms=False, imgsz=640, simplify=True))"
}

$needOrt = -not (Test-Path (Join-Path $OrtDir "lib\onnxruntime.dll"))
if ($needOrt) {
    $zip = Join-Path $env:TEMP "onnxruntime-win-x64-$OrtVersion.zip"
    Download-File "https://github.com/microsoft/onnxruntime/releases/download/v$OrtVersion/onnxruntime-win-x64-$OrtVersion.zip" $zip
    $stage = Join-Path $env:TEMP "onnxruntime-extract-$OrtVersion"
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    Expand-Archive $zip -DestinationPath $stage -Force
    $extracted = Get-ChildItem $stage -Directory | Where-Object { $_.Name -like "onnxruntime*" } | Select-Object -First 1
    if (-not $extracted) { throw "ONNX Runtime archive layout unexpected" }
    Remove-Item $OrtDir -Recurse -Force -ErrorAction SilentlyContinue
    Move-Item $extracted.FullName $OrtDir
}

Write-Host "YOLO model: $onnx"
Write-Host "ONNX Runtime: $OrtDir"
Write-Host "Reconfigure/rebuild upPlayer after this script."
