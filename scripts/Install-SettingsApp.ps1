<#
.SYNOPSIS
  Installs just "WebRTC Audio Engine Settings" from the build folder (admin), for quick app updates while
  developing; Install-Package.ps1 installs it as part of the driver. Runs the app's own installer
  ("/install"): Program Files, Start-menu shortcut, tray icon at every sign-in, Apps & features entry,
  settings key writable by Users. Undo with -Uninstall (settings are kept).
#>
param([string]$ExePath, [switch]$Uninstall)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ApoConfig.ps1')
Assert-Admin
$exe = Join-Path $env:ProgramFiles 'WebrtcAudioEngine\WebrtcAudioEngineSettings.exe'
$log = Join-Path $env:ProgramData 'WebrtcAudioEngine\setup.log'

if ($Uninstall) {
    if (-not (Test-Path $exe)) { Write-Host 'Settings app is not installed.'; return }
    $p = Start-Process $exe -ArgumentList '/uninstall' -PassThru; $p.WaitForExit()  # not -Wait: that also waits for the tray app it starts
    if ($p.ExitCode) { throw "Uninstall failed (code $($p.ExitCode)); see $log" }
    Write-Host 'Settings app removed (settings themselves are kept).'
    return
}

if (-not $ExePath) { $ExePath = Join-Path $WorkspaceRoot 'build\Release\WebrtcAudioEngineSettings.exe' }
if (-not (Test-Path $ExePath)) { throw "Not found: $ExePath (build first)" }
$p = Start-Process $ExePath -ArgumentList '/install dev' -PassThru; $p.WaitForExit()  # not -Wait: that also waits for the tray app it starts
if ($p.ExitCode) { throw "Install failed (code $($p.ExitCode)); see $log" }
& (Join-Path $PSScriptRoot 'Set-VoiceSettings.ps1')
Write-Host "Installed $exe, the Start-menu shortcut and the tray icon (also starts at sign-in)."
