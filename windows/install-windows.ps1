[CmdletBinding()]
param(
    [string]$InstallDirectory = (Join-Path $env:LOCALAPPDATA 'Programs\drcom4scut'),
    [string]$NpcapInstaller = '',
    [switch]$NoShortcut
)

$ErrorActionPreference = 'Stop'
$NpcapDownloadUrl = 'https://npcap.com/#download'

function Test-NpcapInstalled {
    $service = Get-Service -Name 'npcap' -ErrorAction SilentlyContinue
    $wpcap = Test-Path -LiteralPath "$env:WINDIR\System32\Npcap\wpcap.dll"
    $packet = Test-Path -LiteralPath "$env:WINDIR\System32\Npcap\Packet.dll"
    return ($null -ne $service -and $wpcap -and $packet)
}

function Find-ProgramFile([string]$Name) {
    $besideInstaller = Join-Path $PSScriptRoot $Name
    if (Test-Path -LiteralPath $besideInstaller) { return $besideInstaller }

    $repoBinary = Join-Path (Split-Path -Parent $PSScriptRoot) "bin\$Name"
    if (Test-Path -LiteralPath $repoBinary) { return $repoBinary }

    throw "$Name was not found beside this installer or in the repository bin directory."
}

if (Test-NpcapInstalled) {
    Write-Host 'Npcap is already installed. Skipping Npcap installation.' -ForegroundColor Green
} else {
    if (-not $NpcapInstaller) {
        $candidate = Get-ChildItem -LiteralPath $PSScriptRoot -Filter 'npcap-*.exe' -File -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($candidate) { $NpcapInstaller = $candidate.FullName }
    }

    if ($NpcapInstaller) {
        $NpcapInstaller = [IO.Path]::GetFullPath($NpcapInstaller)
        if (-not (Test-Path -LiteralPath $NpcapInstaller)) {
            throw "Npcap installer was not found: $NpcapInstaller"
        }
        Write-Host 'Npcap is missing. Starting the supplied Npcap installer...'
        $process = Start-Process -FilePath $NpcapInstaller -Verb RunAs -Wait -PassThru
        if ($process.ExitCode -ne 0 -or -not (Test-NpcapInstalled)) {
            throw 'Npcap installation did not complete successfully. Install Npcap and run this script again.'
        }
    } else {
        Write-Warning 'Npcap is required but is not installed.'
        Write-Host 'The official download page will be opened. Install Npcap, then run this installer again.'
        Start-Process $NpcapDownloadUrl
        exit 2
    }
}

$cli = Find-ProgramFile 'drcom4scut.exe'
$gui = Find-ProgramFile 'drcom4scut-gui.exe'
New-Item -ItemType Directory -Force -Path $InstallDirectory | Out-Null
Copy-Item -LiteralPath $cli -Destination (Join-Path $InstallDirectory 'drcom4scut.exe') -Force
Copy-Item -LiteralPath $gui -Destination (Join-Path $InstallDirectory 'drcom4scut-gui.exe') -Force

$readme = Join-Path $PSScriptRoot 'README-WINDOWS.md'
if (Test-Path -LiteralPath $readme) {
    Copy-Item -LiteralPath $readme -Destination (Join-Path $InstallDirectory 'README-WINDOWS.md') -Force
}

if (-not $NoShortcut) {
    $startMenu = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
    $shortcutPath = Join-Path $startMenu 'DrCOM4SCUT.lnk'
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = Join-Path $InstallDirectory 'drcom4scut-gui.exe'
    $shortcut.WorkingDirectory = $InstallDirectory
    $shortcut.Description = 'DrCOM4SCUT Windows authentication client'
    $shortcut.Save()
}

Write-Host "DrCOM4SCUT was installed to: $InstallDirectory" -ForegroundColor Green
Write-Host 'Run drcom4scut-gui.exe from the Start menu or installation directory.'
