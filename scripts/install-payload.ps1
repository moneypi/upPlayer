# Payload installer invoked by the self-extracting setup.
param(
    [string]$AppVersion = "1.0.0"
)

$ErrorActionPreference = "Stop"
$Source = Split-Path -Parent $MyInvocation.MyCommand.Path
$AppName = "upPlayer"
$InstallRoot = Join-Path ${env:ProgramFiles} $AppName
$ExeName = "upPlayer.exe"

Write-Host "Installing $AppName $AppVersion to $InstallRoot"

New-Item -ItemType Directory -Force -Path $InstallRoot | Out-Null
Get-ChildItem $Source -Force | Where-Object {
    $_.Name -notin @('install-payload.ps1', 'install.cmd')
} | ForEach-Object {
    $dest = Join-Path $InstallRoot $_.Name
    if ($_.PSIsContainer) {
        Copy-Item $_.FullName $dest -Recurse -Force
    } else {
        Copy-Item $_.FullName $dest -Force
    }
}

$exe = Join-Path $InstallRoot $ExeName
if (-not (Test-Path $exe)) {
    throw "Missing $exe after copy."
}

$programs = [Environment]::GetFolderPath('CommonPrograms')
$startMenu = Join-Path $programs $AppName
New-Item -ItemType Directory -Force -Path $startMenu | Out-Null

$wsh = New-Object -ComObject WScript.Shell
$shortcut = $wsh.CreateShortcut((Join-Path $startMenu "$AppName.lnk"))
$shortcut.TargetPath = $exe
$shortcut.WorkingDirectory = $InstallRoot
$shortcut.IconLocation = "$exe,0"
$shortcut.Save()

$uninstallPs1 = Join-Path $InstallRoot "uninstall.ps1"
@"
`$ErrorActionPreference = 'Stop'
`$root = Split-Path -Parent `$MyInvocation.MyCommand.Path
Remove-Item (Join-Path ([Environment]::GetFolderPath('CommonPrograms')) '$AppName') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$AppName' -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item `$root -Recurse -Force
"@ | Set-Content -Path $uninstallPs1 -Encoding UTF8

$uninstallCmd = Join-Path $InstallRoot "uninstall.cmd"
@"
@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1"
"@ | Set-Content -Path $uninstallCmd -Encoding ASCII

$regPath = "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$AppName"
New-Item -Path $regPath -Force | Out-Null
New-ItemProperty -Path $regPath -Name "DisplayName" -Value $AppName -PropertyType String -Force | Out-Null
New-ItemProperty -Path $regPath -Name "DisplayVersion" -Value $AppVersion -PropertyType String -Force | Out-Null
New-ItemProperty -Path $regPath -Name "Publisher" -Value $AppName -PropertyType String -Force | Out-Null
New-ItemProperty -Path $regPath -Name "InstallLocation" -Value $InstallRoot -PropertyType String -Force | Out-Null
New-ItemProperty -Path $regPath -Name "DisplayIcon" -Value $exe -PropertyType String -Force | Out-Null
New-ItemProperty -Path $regPath -Name "UninstallString" -Value "`"$uninstallCmd`"" -PropertyType String -Force | Out-Null
New-ItemProperty -Path $regPath -Name "NoModify" -Value 1 -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $regPath -Name "NoRepair" -Value 1 -PropertyType DWord -Force | Out-Null
$sizeKb = [int]((Get-ChildItem $InstallRoot -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1KB)
New-ItemProperty -Path $regPath -Name "EstimatedSize" -Value $sizeKb -PropertyType DWord -Force | Out-Null

Write-Host "Installed. Launching $AppName..."
Start-Process -FilePath $exe -WorkingDirectory $InstallRoot
