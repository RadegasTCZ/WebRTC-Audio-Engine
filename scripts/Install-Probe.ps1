<#
.SYNOPSIS
  Installs the WebRTC voice probe APO on one capture endpoint (registry route, test machines only).
.DESCRIPTION
  1. Backs up the endpoint's effect settings (first run keeps a permanent "original" backup).
  2. Copies the DLL to Program Files and registers our two COM classes + APO entries.
  3. APPENDS our CLSIDs after Realtek's in the composite MFX and/or EFX lists. Nothing is removed.
  4. Restarts Windows Audio.
  Undo everything with Uninstall-Probe.ps1.
.EXAMPLE
  .\Install-Probe.ps1 -DryRun        # show what would change, change nothing
  .\Install-Probe.ps1                # install into MFX + EFX
  .\Install-Probe.ps1 -Slots EFX     # endpoint slot only
#>
[CmdletBinding()]
param(
    [string]$EndpointId,
    [ValidateSet('MFX', 'EFX')][string[]]$Slots = @('MFX', 'EFX'),
    [string]$DllPath,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ApoConfig.ps1')
if (-not $EndpointId) { $EndpointId = $DefaultEndpointId }
if (-not $DllPath)    { $DllPath = $DefaultDll }
Assert-Admin

if (-not (Test-Path $DllPath)) { throw "DLL not found: $DllPath (build first)" }
$fx = Open-EndpointSubKey $EndpointId 'FxProperties'
$props = Open-EndpointSubKey $EndpointId 'Properties'
$name = $props.GetValue('{a45c254e-df1c-4efd-8020-67d146a850e0},2')

$targets = @()
if ($Slots -contains 'MFX') { $targets += [pscustomobject]@{ Slot = 'MFX'; Value = $PKEY_CompositeMfx; Clsid = $ClsidMfx } }
if ($Slots -contains 'EFX') { $targets += [pscustomobject]@{ Slot = 'EFX'; Value = $PKEY_CompositeEfx; Clsid = $ClsidEfx } }

Write-Host "Endpoint : $name  $EndpointId"
Write-Host "DLL      : $DllPath -> $InstallDir"
foreach ($t in $targets) {
    $cur = Get-MultiSz $fx $t.Value
    $new = @($cur | Where-Object { $_ }) ; if ($new -notcontains $t.Clsid) { $new += $t.Clsid }
    Write-Host "$($t.Slot) list ($($t.Value)):"
    Write-Host "   now : $(if ($cur) { $cur -join ', ' } else { '(not set)' })"
    Write-Host "   new : $($new -join ', ')"
}
$sysfx = $props.GetValue($PKEY_DisableSysFx)
Write-Host "Disable_SysFx on endpoint: $(if ($null -eq $sysfx) { '(not set)' } else { $sysfx })"
if ($DryRun) { Write-Host "`nDry run: nothing was changed."; return }

# ---- 1. Backups
New-Item -ItemType Directory -Force $BackupDir | Out-Null
$epShort = $EndpointId.Trim('{}')
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$fxRegPath = "HKLM\$(Get-EndpointKeyPath $EndpointId)\FxProperties"
& reg.exe export $fxRegPath (Join-Path $BackupDir "FxProperties-$epShort-$stamp.reg") /y | Out-Null
$originalJson = Join-Path $BackupDir "original-$epShort.json"
if (-not (Test-Path $originalJson)) {
    [pscustomobject]@{
        EndpointId = $EndpointId; EndpointName = $name; SavedAt = (Get-Date).ToString('s')
        CompositeMfx = Get-MultiSz $fx $PKEY_CompositeMfx
        CompositeEfx = Get-MultiSz $fx $PKEY_CompositeEfx
        DisableSysFx = $sysfx
    } | ConvertTo-Json | Set-Content $originalJson -Encoding utf8
    Copy-Item (Join-Path $BackupDir "FxProperties-$epShort-$stamp.reg") (Join-Path $BackupDir "original-FxProperties-$epShort.reg")
    Write-Host "Saved ORIGINAL settings: $originalJson"
}

# ---- 2. Files (audio stopped so audiodg releases any old copy of the DLL)
Stop-Audio
try {
    New-Item -ItemType Directory -Force $InstallDir | Out-Null
    Copy-Item $DllPath (Join-Path $InstallDir 'WebrtcVoiceApo.dll') -Force
    $dll = Join-Path $InstallDir 'WebrtcVoiceApo.dll'

    New-Item -ItemType Directory -Force $LogDir | Out-Null
    # audiodg runs as LOCAL SERVICE (sometimes in a restricted/AppContainer-like token): let it append logs.
    & icacls.exe $LogDir /grant '*S-1-5-19:(OI)(CI)M' '*S-1-15-2-1:(OI)(CI)M' '*S-1-1-0:(OI)(CI)M' | Out-Null

    # ---- 3. COM + APO registration
    $classes = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey('SOFTWARE\Classes')
    foreach ($c in @(@($ClsidMfx, 'WebRTC Voice Probe (MFX)'), @($ClsidEfx, 'WebRTC Voice Probe (EFX)'))) {
        $k = $classes.CreateSubKey("CLSID\$($c[0])"); $k.SetValue('', $c[1])
        $ip = $k.CreateSubKey('InprocServer32'); $ip.SetValue('', $dll); $ip.SetValue('ThreadingModel', 'Both')
        $ip.Close(); $k.Close()

        $a = $classes.CreateSubKey("AudioEngine\AudioProcessingObjects\$($c[0])")
        $a.SetValue('FriendlyName', $c[1]);              $a.SetValue('Copyright', 'Copyright (c) RadegasTCZ')
        $a.SetValue('MajorVersion', 1, 'DWord');        $a.SetValue('MinorVersion', 0, 'DWord')
        $a.SetValue('Flags', 0x0E, 'DWord')               # APO_FLAG_DEFAULT
        $a.SetValue('MinInputConnections', 1, 'DWord');  $a.SetValue('MaxInputConnections', 1, 'DWord')
        $a.SetValue('MinOutputConnections', 1, 'DWord'); $a.SetValue('MaxOutputConnections', 1, 'DWord')
        $a.SetValue('MaxInstances', -1, 'DWord')          # 0xFFFFFFFF
        $a.SetValue('NumAPOInterfaces', 1, 'DWord');     $a.SetValue('APOInterface0', $ApoInterfaceIid)
        $a.Close()
    }
    $classes.Close()

    # ---- 4. Append to the endpoint's composite lists
    foreach ($t in $targets) {
        $list = @(Get-MultiSz $fx $t.Value | Where-Object { $_ })
        if ($list -notcontains $t.Clsid) { $list += $t.Clsid }
        $fx.SetValue($t.Value, [string[]]$list, 'MultiString')
        Write-Host "$($t.Slot): registered."
    }
}
finally {
    $fx.Close(); $props.Close()
    Start-Audio
}
Write-Host "`nInstalled. Log: $LogDir\probe.log  (collect with .\Collect-ProbeLog.ps1)"
Write-Host 'Open any app that records from the mic, then check the log.'
