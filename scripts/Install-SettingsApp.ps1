<#
.SYNOPSIS
  Installs "WebRTC Audio Engine Settings" (admin): copies the app to Program Files, adds a Start-menu
  shortcut for all users, and makes sure the settings key exists and is writable by Users.
  Undo with -Uninstall.
#>
param([string]$ExePath, [switch]$Uninstall)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ApoConfig.ps1')
Assert-Admin
$dir = Join-Path $env:ProgramFiles 'WebrtcAudioEngine'
$exe = Join-Path $dir 'WebrtcAudioEngineSettings.exe'
$lnk = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\WebRTC Audio Engine Settings.lnk'

if ($Uninstall) {
    Get-Process WebrtcAudioEngineSettings -EA SilentlyContinue | Stop-Process -Force
    Remove-Item $lnk -Force -EA SilentlyContinue
    Remove-Item $dir -Recurse -Force -EA SilentlyContinue
    Write-Host 'Settings app removed (settings themselves are kept).'
    return
}

if (-not $ExePath) { $ExePath = Join-Path $WorkspaceRoot 'build\Release\WebrtcAudioEngineSettings.exe' }
if (-not (Test-Path $ExePath)) { throw "Not found: $ExePath (build first)" }
Get-Process WebrtcAudioEngineSettings -EA SilentlyContinue | Stop-Process -Force
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item $ExePath $exe -Force

$shell = New-Object -ComObject WScript.Shell
$s = $shell.CreateShortcut($lnk)
$s.TargetPath = $exe
$s.Description = 'Echo cancellation, noise suppression, automatic gain and cleanup for your microphone'
$s.Save()

# Settings key with Users write access (Set-VoiceSettings creates it when missing).
& (Join-Path $PSScriptRoot 'Set-VoiceSettings.ps1') | Out-Null
Write-Host "Installed $exe and the Start-menu shortcut 'WebRTC Audio Engine Settings'."
