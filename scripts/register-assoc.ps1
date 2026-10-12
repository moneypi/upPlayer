# Register / unregister upPlayer for Windows "Open with" and Default Apps (PotPlayer/VLC style).
param(
    [ValidateSet('Register', 'Unregister')]
    [string]$Action = 'Register',
    [string]$ExePath = "",
    [string]$AppName = 'upPlayer'
)

$ErrorActionPreference = 'Stop'

$Extensions = @(
    '.mp4', '.m4v', '.mkv', '.webm', '.avi', '.mov', '.wmv', '.flv', '.ts', '.m2ts', '.mts',
    '.mpg', '.mpeg', '.mpe', '.vob', '.rmvb', '.rm', '.3gp', '.3g2', '.asf', '.f4v', '.ogv',
    '.divx', '.wtv', '.dvr-ms', '.m3u8', '.m3u'
)

function Get-ExePath {
    param([string]$Path)
    if ($Path -and (Test-Path -LiteralPath $Path)) {
        return (Resolve-Path -LiteralPath $Path).Path
    }
    $candidates = @(
        (Join-Path ${env:ProgramFiles} "$AppName\$AppName.exe"),
        (Join-Path $PSScriptRoot "$AppName.exe")
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { return (Resolve-Path -LiteralPath $c).Path }
    }
    throw "upPlayer.exe not found. Pass -ExePath."
}

function Ensure-Key([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -Path $Path -Force | Out-Null
    }
}

function Set-String([string]$Path, [string]$Name, [string]$Value) {
    Ensure-Key $Path
    $valueName = if ($Name -eq '' -or $Name -eq '(default)') { '(default)' } else { $Name }
    if (Get-ItemProperty -LiteralPath $Path -Name $valueName -ErrorAction SilentlyContinue) {
        Set-ItemProperty -LiteralPath $Path -Name $valueName -Value $Value -Force
    } else {
        New-ItemProperty -LiteralPath $Path -Name $valueName -Value $Value -PropertyType String -Force | Out-Null
    }
}

function Set-None([string]$Path, [string]$Name) {
    Ensure-Key $Path
    # REG_NONE empty value used by OpenWithProgids
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
        ($Path -replace '^HKLM:\\', ''), $true)
    if (-not $key) {
        $key = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey(($Path -replace '^HKLM:\\', ''))
    }
    $key.SetValue($Name, [byte[]]@(), [Microsoft.Win32.RegistryValueKind]::None)
    $key.Close()
}

function Notify-Shell {
    try {
        Add-Type -Namespace Native -Name Shell32 -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("shell32.dll")]
public static extern void SHChangeNotify(int wEventId, uint uFlags, System.IntPtr dwItem1, System.IntPtr dwItem2);
"@ -ErrorAction SilentlyContinue
        # SHCNE_ASSOCCHANGED = 0x08000000, SHCNF_IDLIST = 0
        [Native.Shell32]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
    } catch {}
}

function Register-UpPlayer([string]$Exe) {
    $exeName = Split-Path $Exe -Leaf
    $cmd = "`"$Exe`" `"%1`""
    $icon = "`"$Exe`",0"

    # App Paths (Start/Run and shell resolution)
    Set-String "HKLM:\Software\Microsoft\Windows\CurrentVersion\App Paths\$exeName" '(default)' $Exe
    Set-String "HKLM:\Software\Microsoft\Windows\CurrentVersion\App Paths\$exeName" 'Path' (Split-Path $Exe -Parent)

    # Applications\* — primary "Open with" listing
    $appKey = "HKLM:\Software\Classes\Applications\$exeName"
    Set-String $appKey 'FriendlyAppName' $AppName
    Set-String "$appKey\shell\open" '(default)' "Play with $AppName"
    Set-String "$appKey\shell\open\command" '(default)' $cmd
    foreach ($ext in $Extensions) {
        Set-String "$appKey\SupportedTypes" $ext ''
    }

    # Default Apps / "Choose default apps by file type" (Clients\Media like PotPlayer/VLC)
    $capRoot = "HKLM:\Software\Clients\Media\$AppName"
    Set-String "$capRoot\Capabilities" 'ApplicationName' $AppName
    Set-String "$capRoot\Capabilities" 'ApplicationDescription' 'Lightweight Win32 front-end for mpv'
    foreach ($ext in $Extensions) {
        $progId = "$AppName$ext"  # e.g. upPlayer.mp4
        Set-String "$capRoot\Capabilities\FileAssociations" $ext $progId

        Set-String "HKLM:\Software\Classes\$progId" '(default)' "$AppName Video"
        Set-String "HKLM:\Software\Classes\$progId\DefaultIcon" '(default)' $icon
        Set-String "HKLM:\Software\Classes\$progId\shell" '(default)' 'open'
        Set-String "HKLM:\Software\Classes\$progId\shell\open" '(default)' "Play with $AppName"
        Set-String "HKLM:\Software\Classes\$progId\shell\open" 'MultiSelectModel' 'Player'
        Set-String "HKLM:\Software\Classes\$progId\shell\open\command" '(default)' $cmd

        # Offer in Open with for the extension
        Set-None "HKLM:\Software\Classes\$ext\OpenWithProgids" $progId
        Set-String "HKLM:\Software\Classes\$ext\OpenWithList\$exeName" '(default)' ''
    }

    Ensure-Key 'HKLM:\Software\RegisteredApplications'
    New-ItemProperty -LiteralPath 'HKLM:\Software\RegisteredApplications' `
        -Name $AppName -Value "Software\Clients\Media\$AppName\Capabilities" `
        -PropertyType String -Force | Out-Null

    Notify-Shell
    Write-Host "Registered $AppName for Open with / Default Apps ($($Extensions.Count) types)."
}

function Unregister-UpPlayer([string]$Exe) {
    $exeName = if ($Exe) { Split-Path $Exe -Leaf } else { "$AppName.exe" }

    Remove-Item -LiteralPath "HKLM:\Software\Microsoft\Windows\CurrentVersion\App Paths\$exeName" -Recurse -Force -EA SilentlyContinue
    Remove-Item -LiteralPath "HKLM:\Software\Classes\Applications\$exeName" -Recurse -Force -EA SilentlyContinue
    Remove-Item -LiteralPath "HKLM:\Software\Clients\Media\$AppName" -Recurse -Force -EA SilentlyContinue
    Remove-ItemProperty -LiteralPath 'HKLM:\Software\RegisteredApplications' -Name $AppName -Force -EA SilentlyContinue

    foreach ($ext in $Extensions) {
        $progId = "$AppName$ext"
        Remove-Item -LiteralPath "HKLM:\Software\Classes\$progId" -Recurse -Force -EA SilentlyContinue
        Remove-ItemProperty -LiteralPath "HKLM:\Software\Classes\$ext\OpenWithProgids" -Name $progId -Force -EA SilentlyContinue
        Remove-Item -LiteralPath "HKLM:\Software\Classes\$ext\OpenWithList\$exeName" -Recurse -Force -EA SilentlyContinue
    }

    Notify-Shell
    Write-Host "Unregistered $AppName file associations."
}

if ($Action -eq 'Register') {
    Register-UpPlayer (Get-ExePath -Path $ExePath)
} else {
    $exe = $null
    try { $exe = Get-ExePath -Path $ExePath } catch {}
    Unregister-UpPlayer $exe
}
