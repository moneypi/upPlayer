# Build upPlayer-Setup-x64.exe with NSIS (same installer family as PotPlayer).
# Falls back to Inno Setup or the self-extracting stub if makensis is unavailable.
param(
    [string]$Configuration = "Release",
    [string]$OutDir = "",
    [string]$ReleaseTag = "latest",
    [string]$AppVersion = "",
    [switch]$SkipPackage,
    [switch]$NsisOnly,
    [switch]$InnoOnly,
    [switch]$SfxOnly
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

function Get-UpPlayerVersion {
    $header = Join-Path $Root "src\version.hpp"
    if (-not (Test-Path $header)) { return "0.0.0" }
    $text = Get-Content $header -Raw
    if ($text -match 'UPPLAYER_VERSION_STRING\s+"([^"]+)"') {
        return $Matches[1]
    }
    return "0.0.0"
}

if (-not $AppVersion) {
    $AppVersion = Get-UpPlayerVersion
}
Write-Host "AppVersion=$AppVersion"

if (-not $OutDir) {
    $OutDir = Join-Path $Root "build\dist"
}
$PkgDir = Join-Path $OutDir "package"
$Nsi = Join-Path $PSScriptRoot "upPlayer.nsi"
$Iss = Join-Path $PSScriptRoot "upPlayer.iss"
$SetupOut = Join-Path $OutDir "upPlayer-Setup-x64-$AppVersion.exe"

function Find-MakeNsis {
    $candidates = @(
        "$env:LOCALAPPDATA\Programs\upplayer-toolchain\nsis-3.13\makensis.exe",
        "$env:LOCALAPPDATA\Programs\upplayer-toolchain\nsis-3.13\Bin\makensis.exe",
        "${env:ProgramFiles}\NSIS\makensis.exe",
        "${env:ProgramFiles(x86)}\NSIS\makensis.exe",
        "$env:LOCALAPPDATA\Programs\NSIS\makensis.exe"
    )
    foreach ($p in $candidates) {
        if (Test-Path $p) { return $p }
    }
    $cmd = Get-Command makensis.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Find-ISCC {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 7\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 7\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 7\ISCC.exe"
    )
    foreach ($p in $candidates) {
        if (Test-Path $p) { return $p }
    }
    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Find-Csc {
    $candidates = @(
        "${env:WINDIR}\Microsoft.NET\Framework64\v4.0.30319\csc.exe",
        "${env:WINDIR}\Microsoft.NET\Framework\v4.0.30319\csc.exe"
    )
    foreach ($p in $candidates) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

function Install-Nsis {
    $portableRoot = Join-Path $env:LOCALAPPDATA "Programs\upplayer-toolchain\nsis-3.13"
    $zipCandidates = @(
        (Join-Path $env:USERPROFILE "Downloads\nsis-3.13.zip"),
        (Join-Path $env:TEMP "nsis-3.13.zip"),
        (Join-Path $Root "third_party\nsis\nsis-3.13.zip")
    )
    $zip = $zipCandidates | Where-Object { (Test-Path $_) -and ((Get-Item $_).Length -gt 1MB) } | Select-Object -First 1

    if (-not $zip) {
        $zip = Join-Path $env:TEMP "nsis-3.13.zip"
        $urls = @(
            "https://sourceforge.net/projects/nsis/files/NSIS%203/3.13/nsis-3.13.zip/download",
            "https://downloads.sourceforge.net/project/nsis/NSIS%203/3.13/nsis-3.13.zip",
            "https://zenlayer.dl.sourceforge.net/project/nsis/NSIS%203/3.13/nsis-3.13.zip"
        )
        foreach ($url in $urls) {
            Write-Host "Downloading NSIS from $url"
            Remove-Item $zip -Force -ErrorAction SilentlyContinue
            & curl.exe -fL --ssl-no-revoke --noproxy "*" --connect-timeout 30 --retry 3 --retry-all-errors `
                -A "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36" `
                -e "https://sourceforge.net/" -o $zip $url
            if ((Test-Path $zip) -and ((Get-Item $zip).Length -gt 1MB)) {
                break
            }
        }
    }

    if (-not $zip -or -not (Test-Path $zip) -or ((Get-Item $zip).Length -lt 1MB)) {
        throw "Failed to get NSIS. Download nsis-3.13.zip from https://nsis.sourceforge.io/Download into Downloads\, or install NSIS.NSIS (winget)."
    }

    Write-Host "Extracting portable NSIS from $zip"
    $parent = Split-Path $portableRoot
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    Remove-Item $portableRoot -Recurse -Force -ErrorAction SilentlyContinue
    Expand-Archive -Path $zip -DestinationPath $parent -Force
    if (-not (Test-Path (Join-Path $portableRoot "makensis.exe"))) {
        $found = Get-ChildItem $parent -Directory -Filter "nsis*" |
            Where-Object { Test-Path (Join-Path $_.FullName "makensis.exe") } |
            Select-Object -First 1
        if ($found -and $found.FullName -ne $portableRoot) {
            Rename-Item $found.FullName (Split-Path $portableRoot -Leaf)
        }
    }
    if (-not (Find-MakeNsis)) {
        throw "makensis.exe missing after extracting $zip"
    }
}

function Build-NsisSetup {
    $makensis = Find-MakeNsis
    if (-not $makensis) {
        Install-Nsis
        $makensis = Find-MakeNsis
    }
    if (-not $makensis) {
        throw "makensis.exe not found after installing NSIS."
    }
    if (-not (Test-Path $Nsi)) {
        throw "Missing $Nsi"
    }

    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    Write-Host "Compiling NSIS setup with $makensis"
    & $makensis `
        "/DSOURCE_DIR=$PkgDir" `
        "/DOUT_DIR=$OutDir" `
        "/DAPP_VERSION=$AppVersion" `
        $Nsi
    if ($LASTEXITCODE -ne 0) {
        throw "makensis failed with exit code $LASTEXITCODE"
    }
    if (-not (Test-Path $SetupOut)) {
        throw "Expected output missing: $SetupOut"
    }
    return $true
}

function Build-InnoSetup {
    $iscc = Find-ISCC
    if (-not $iscc) { return $false }
    if (-not (Test-Path $Iss)) { throw "Missing $Iss" }

    Write-Host "Compiling Inno Setup with $iscc"
    & $iscc `
        "/DSourceDir=$PkgDir" `
        "/DOutputDir=$OutDir" `
        "/DAppVersion=$AppVersion" `
        $Iss
    if ($LASTEXITCODE -ne 0) {
        throw "ISCC failed with exit code $LASTEXITCODE"
    }
    if (-not (Test-Path $SetupOut)) {
        throw "Expected output missing: $SetupOut"
    }
    return $true
}

function Build-SfxSetup {
    $csc = Find-Csc
    if (-not $csc) {
        throw "csc.exe not found. Install NSIS, or keep the .NET Framework 4 csc toolchain."
    }

    $stage = Join-Path $OutDir "setup-stage"
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $stage | Out-Null

    Copy-Item (Join-Path $PkgDir '*') $stage -Recurse -Force
    Copy-Item (Join-Path $PSScriptRoot 'install-payload.ps1') $stage -Force

    $payloadZip = Join-Path $OutDir "setup-payload.zip"
    Remove-Item $payloadZip -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $payloadZip -Force

    $stubCs = Join-Path $PSScriptRoot "SetupStub.cs"
    $stubExe = Join-Path $OutDir "setup-stub.exe"
    Remove-Item $stubExe -Force -ErrorAction SilentlyContinue

    Write-Host "Compiling setup stub with $csc"
    & $csc /nologo /target:winexe /optimize+ /platform:anycpu `
        /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll /r:System.Windows.Forms.dll `
        /out:$stubExe $stubCs
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $stubExe)) {
        throw "Failed to compile setup stub."
    }

    Write-Host "Packing self-extracting setup..."
    Remove-Item $SetupOut -Force -ErrorAction SilentlyContinue
    $stubBytes = [IO.File]::ReadAllBytes($stubExe)
    $zipBytes = [IO.File]::ReadAllBytes($payloadZip)
    $zipOffset = [int64]$stubBytes.Length
    $magic = [int64]0x5550504C53544658
    $trailer = New-Object byte[] 16
    [BitConverter]::GetBytes($zipOffset).CopyTo($trailer, 0)
    [BitConverter]::GetBytes($magic).CopyTo($trailer, 8)

    $out = New-Object byte[] ($stubBytes.Length + $zipBytes.Length + 16)
    [Buffer]::BlockCopy($stubBytes, 0, $out, 0, $stubBytes.Length)
    [Buffer]::BlockCopy($zipBytes, 0, $out, $stubBytes.Length, $zipBytes.Length)
    [Buffer]::BlockCopy($trailer, 0, $out, $stubBytes.Length + $zipBytes.Length, 16)
    [IO.File]::WriteAllBytes($SetupOut, $out)

    Remove-Item $stubExe, $payloadZip -Force -ErrorAction SilentlyContinue
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    return $true
}

if (-not $SkipPackage) {
    & (Join-Path $PSScriptRoot "package-release.ps1") `
        -Configuration $Configuration `
        -OutDir $OutDir `
        -ReleaseTag $ReleaseTag
}

if (-not (Test-Path (Join-Path $PkgDir "upPlayer.exe"))) {
    throw "Missing $PkgDir\upPlayer.exe. Run package-release.ps1 first, or omit -SkipPackage."
}

$built = $false
if (-not $InnoOnly -and -not $SfxOnly) {
    try {
        $built = Build-NsisSetup
    } catch {
        if ($NsisOnly) { throw }
        Write-Host "NSIS unavailable ($($_.Exception.Message)); trying fallbacks."
    }
}

if (-not $built -and -not $NsisOnly -and -not $SfxOnly) {
    try {
        $built = Build-InnoSetup
    } catch {
        if ($InnoOnly) { throw }
        Write-Host "Inno Setup unavailable ($($_.Exception.Message)); trying SFX fallback."
    }
}

if (-not $built) {
    if ($NsisOnly -or $InnoOnly) {
        throw "Requested installer toolchain was not available."
    }
    [void](Build-SfxSetup)
}

Write-Host "setup: $SetupOut ($((Get-Item $SetupOut).Length) bytes)"
