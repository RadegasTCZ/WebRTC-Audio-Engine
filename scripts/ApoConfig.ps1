# Shared settings for the scripts. Dot-source this file.

$Script:LogDir = Join-Path $env:ProgramData 'WebrtcVoiceApo'   # the effect's log (engine.log)

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

# Removes the installed settings app (settings are kept). Runs the uninstaller from a temporary copy:
# the copy in Program Files cannot delete itself.
function Uninstall-SettingsApp {
    if (-not (Test-Path $InstalledAppExe)) { Write-Host 'Settings app is not installed.'; return }
    $tmp = Join-Path $env:TEMP "WebrtcAudioEngine-uninstall-$PID.exe"
    Copy-Item $InstalledAppExe $tmp -Force
    try { $code = Invoke-AppSetup $tmp '/uninstall' } finally { Remove-Item $tmp -Force -EA SilentlyContinue }
    if ($code) { throw "Removing the settings app failed (code $code); see $SetupLog" }
    Write-Host 'Settings app removed (settings are kept).'
}

function Assert-Admin {
    $p = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this from an administrator PowerShell.'
    }
}

# Log folder for the effect inside audiodg. audiodg runs as LOCAL SERVICE, sometimes with an
# AppContainer-like token (S-1-15-2-1 ALL APPLICATION PACKAGES, S-1-15-2-2 ALL RESTRICTED APPLICATION
# PACKAGES), so those may write. Users may only read: a folder that ordinary users can write into while a
# service writes files there invites link/redirect tricks. Inheritance from ProgramData (which lets Users
# create files) is removed.
function Initialize-LogDir {
    New-Item -ItemType Directory -Force $LogDir | Out-Null
    & icacls.exe $LogDir /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-19:(OI)(CI)M' `
        '*S-1-15-2-1:(OI)(CI)M' '*S-1-15-2-2:(OI)(CI)M' '*S-1-5-32-545:(OI)(CI)RX' | Out-Null
    if ($LASTEXITCODE) { throw "icacls failed on $LogDir" }
    # Drop any other entries left from older installs (e.g. Everyone:Modify).
    & icacls.exe $LogDir /remove:g '*S-1-1-0' '*S-1-3-0' | Out-Null
    if (Get-ChildItem $LogDir -Force) { & icacls.exe (Join-Path $LogDir '*') /reset /T /Q | Out-Null }  # existing files: inherit the new ACL
}

function Stop-Audio { Write-Host 'Stopping Windows Audio service...';  Stop-Service Audiosrv -Force -ErrorAction Stop }
function Start-Audio { Write-Host 'Starting Windows Audio service...'; Start-Service AudioEndpointBuilder -ErrorAction SilentlyContinue; Start-Service Audiosrv -ErrorAction Stop }
