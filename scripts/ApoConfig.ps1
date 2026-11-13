# Shared settings for the scripts. Dot-source this file.

# The effect's log: segmented engine-<start time>.log files in audiodg's (LOCAL SERVICE) temp folder (src\Log.cpp).
$Script:LogDir = Join-Path $env:SystemRoot 'ServiceProfiles\LocalService\AppData\Local\Temp\WebrtcAudioEngine'

# Workspace layout: <workspace>\webrtc_driver\scripts\  ->  <workspace>\build, \logs
$Script:WorkspaceRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Script:DefaultDll    = Join-Path $WorkspaceRoot 'build\Release\WebrtcVoiceApo.dll'
$Script:DefaultAppExe   = Join-Path $WorkspaceRoot 'build\Release\WebrtcAudioEngineSettings.exe'
$Script:InstalledAppExe = Join-Path $env:ProgramFiles 'WebrtcAudioEngine\WebrtcAudioEngineSettings.exe'
$Script:SetupLog        = Join-Path $env:ProgramData 'WebrtcAudioEngine\setup.log'

# Runs the settings app's installer mode and returns its exit code (0 = OK). Start-Process -Wait would also
# wait for the tray app it starts; reading ExitCode needs the process handle taken while it runs.
function Invoke-AppSetup([string]$Exe, [string]$Arguments) {
    $p = Start-Process $Exe -ArgumentList $Arguments -PassThru
    $null = $p.Handle
    $p.WaitForExit()
    $p.ExitCode
}

# Removes the installed settings app (settings are kept). The exe moves itself out of Program Files
# (it is deleted at the next restart).
function Uninstall-SettingsApp {
    if (-not (Test-Path $InstalledAppExe)) { Write-Host 'Settings app is not installed.'; return }
    $code = Invoke-AppSetup $InstalledAppExe '/uninstall'
    if ($code) { throw "Removing the settings app failed (code $code); see $SetupLog" }
    Write-Host 'Settings app removed (settings are kept).'
}

function Assert-Admin {
    $p = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this from an administrator PowerShell.'
    }
}

function Stop-Audio { Write-Host 'Stopping Windows Audio service...';  Stop-Service Audiosrv -Force -ErrorAction Stop }
function Start-Audio { Write-Host 'Starting Windows Audio service...'; Start-Service AudioEndpointBuilder -ErrorAction SilentlyContinue; Start-Service Audiosrv -ErrorAction Stop }
