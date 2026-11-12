# Shared settings for the scripts. Dot-source this file.

$Script:LogDir = Join-Path $env:ProgramData 'WebrtcVoiceApo'   # the effect's log (engine.log)

# Workspace layout: <workspace>\webrtc_driver\scripts\  ->  <workspace>\build, \logs
$Script:WorkspaceRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Script:DefaultDll    = Join-Path $WorkspaceRoot 'build\Release\WebrtcVoiceApo.dll'

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
