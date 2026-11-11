<#
.SYNOPSIS
  Installs "WebRTC Audio Engine Settings" (admin): copies the app to Program Files, adds a Start-menu
  shortcut for all users, starts its tray icon at every sign-in (HKLM Run), and makes sure the settings
  key exists and is writable by Users. Undo with -Uninstall.
#>
param([string]$ExePath, [switch]$Uninstall)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ApoConfig.ps1')
Assert-Admin
$dir = Join-Path $env:ProgramFiles 'WebrtcAudioEngine'
$exe = Join-Path $dir 'WebrtcAudioEngineSettings.exe'
$lnk = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\WebRTC Audio Engine Settings.lnk'
$runKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
$runName = 'WebRTC Audio Engine'

if ($Uninstall) {
    Get-Process WebrtcAudioEngineSettings -EA SilentlyContinue | Stop-Process -Force
    Remove-ItemProperty -Path $runKey -Name $runName -EA SilentlyContinue
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

# Tray icon at every sign-in (all users).
$runValue = '"' + $exe + '" /tray'
Set-ItemProperty -Path $runKey -Name $runName -Value $runValue

# Settings key with Users write access (Set-VoiceSettings creates it when missing).
& (Join-Path $PSScriptRoot 'Set-VoiceSettings.ps1') | Out-Null

Start-Process -FilePath $exe -ArgumentList '/tray'
Write-Host "Installed $exe, the Start-menu shortcut and the tray icon (also starts at sign-in)."
