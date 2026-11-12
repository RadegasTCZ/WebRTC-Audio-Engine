# Shared settings for the install / uninstall scripts. Dot-source this file.
# CLSIDs must match src\Guids.h.

$Script:ClsidMfx = '{50E690BC-E5D5-4D62-84DF-D50D2FD20E16}'
$Script:ClsidEfx = '{F017616C-C6AE-4B1D-942A-D0D1DD5FEB74}'

# Default capture endpoint: "Microphone Array" (Realtek ALC256) on the test laptop.
$Script:DefaultEndpointId = '{6a6941e8-d926-4682-b736-b36eaec76eec}'

$Script:InstallDir = Join-Path $env:ProgramFiles 'WebrtcVoiceApo'
$Script:LogDir     = Join-Path $env:ProgramData  'WebrtcVoiceApo'

# Workspace layout: <workspace>\webrtc_driver\scripts\  ->  <workspace>\backups, \build, \logs
$Script:WorkspaceRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Script:BackupDir     = Join-Path $WorkspaceRoot 'backups'
$Script:DefaultDll    = Join-Path $WorkspaceRoot 'build\Release\WebrtcVoiceApo.dll'

# Endpoint property keys (as registry value names).
$Script:PKEY_CompositeMfx = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14'
$Script:PKEY_CompositeEfx = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15'
$Script:PKEY_DisableSysFx = '{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5'

$Script:ApoInterfaceIid = '{FD7F2B29-24D0-4b5c-B177-592C39F9CA10}'  # IAudioProcessingObject

function Get-EndpointKeyPath([string]$EndpointId) {
    "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\$EndpointId"
}

# Admins only have SetValue+QueryValues on MMDevices keys (owner is SYSTEM), so open with exactly that.
function Open-EndpointSubKey([string]$EndpointId, [string]$Sub) {
    $path = "$(Get-EndpointKeyPath $EndpointId)\$Sub"
    $k = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($path,
            [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
            [System.Security.AccessControl.RegistryRights]'SetValue, QueryValues')
    if (-not $k) { throw "Endpoint key not found: HKLM\$path" }
    $k
}

function Get-MultiSz($Key, [string]$Name) {
    $v = $Key.GetValue($Name, $null, 'DoNotExpandEnvironmentNames')
    if ($null -eq $v) { return $null }
    @($v)
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
