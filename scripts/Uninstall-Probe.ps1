<#
.SYNOPSIS
  Removes the WebRTC voice probe APO and puts the endpoint's effect settings back.
.DESCRIPTION
  Removes ONLY our CLSIDs from the composite MFX/EFX lists (Realtek's entries stay), then
  compares with the original backup and restores any value that differs (incl. Disable_SysFx,
  which Windows sets by itself after repeated APO load failures).
.EXAMPLE
  .\Uninstall-Probe.ps1 -DryRun
  .\Uninstall-Probe.ps1
#>
[CmdletBinding()]
param(
    [string]$EndpointId,
    [switch]$RemoveLogs,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ApoConfig.ps1')
if (-not $EndpointId) { $EndpointId = $DefaultEndpointId }
Assert-Admin

$fx = Open-EndpointSubKey $EndpointId 'FxProperties'
$props = Open-EndpointSubKey $EndpointId 'Properties'
$originalJson = Join-Path $BackupDir "original-$($EndpointId.Trim('{}')).json"
$orig = if (Test-Path $originalJson) { Get-Content $originalJson -Raw | ConvertFrom-Json } else { $null }
if (-not $orig) { Write-Warning "No original backup found at $originalJson; will only remove our entries." }

function Plan-List([string]$valueName, $origList) {
    $cur = Get-MultiSz $fx $valueName
    $new = @($cur | Where-Object { $_ -and $_ -ne $ClsidMfx -and $_ -ne $ClsidEfx })
    if ($orig) { $new = @($origList) }   # original wins if we have it
    [pscustomobject]@{ Name = $valueName; Current = $cur; Target = $new }
}
$plans = @(
    Plan-List $PKEY_CompositeMfx $(if ($orig) { $orig.CompositeMfx })
    Plan-List $PKEY_CompositeEfx $(if ($orig) { $orig.CompositeEfx })
)
foreach ($p in $plans) {
    Write-Host "$($p.Name):"
    Write-Host "   now    : $(if ($p.Current) { $p.Current -join ', ' } else { '(not set)' })"
    Write-Host "   restore: $(if ($p.Target -and $p.Target[0]) { $p.Target -join ', ' } else { '(remove value)' })"
}
$sysfxNow = $fx.GetValue($PKEY_DisableSysFx)   # lives in FxProperties (Settings > Audio enhancements: Off)
$sysfxOrig = if ($orig) { $orig.DisableSysFx } else { $sysfxNow }
Write-Host "Disable_SysFx: now=$(if ($null -eq $sysfxNow) {'(not set)'} else {$sysfxNow})  restore=$(if ($null -eq $sysfxOrig) {'(not set)'} else {$sysfxOrig})"
Write-Host "Remove: COM + APO keys for $ClsidMfx, $ClsidEfx; NS settings HKLM\SOFTWARE\WebrtcVoiceApo; folder $InstallDir$(if ($RemoveLogs) { " and $LogDir" })"
if ($DryRun) { Write-Host "`nDry run: nothing was changed."; return }

Stop-Audio
try {
    foreach ($p in $plans) {
        if ($p.Target -and $p.Target[0]) { $fx.SetValue($p.Name, [string[]]$p.Target, 'MultiString') }
        elseif ($null -ne $p.Current)    { $fx.DeleteValue($p.Name, $false) }
    }
    if ("$sysfxNow" -ne "$sysfxOrig") {
        try {
            if ($null -eq $sysfxOrig) { $fx.DeleteValue($PKEY_DisableSysFx, $false) }
            else { $fx.SetValue($PKEY_DisableSysFx, [int]$sysfxOrig, 'DWord') }
            Write-Host 'Disable_SysFx restored.'
        } catch { Write-Warning "Could not restore Disable_SysFx: $($_.Exception.Message)" }
    }
    # Full paths from HKLM: SOFTWARE\Classes itself can't be opened for writing.
    $hklm = [Microsoft.Win32.Registry]::LocalMachine
    foreach ($c in @($ClsidMfx, $ClsidEfx)) {
        $hklm.DeleteSubKeyTree("SOFTWARE\Classes\CLSID\$c", $false)
        $hklm.DeleteSubKeyTree("SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\$c", $false)
    }
    $hklm.DeleteSubKeyTree('SOFTWARE\WebrtcVoiceApo', $false)   # NS settings (Set-NsSettings.ps1)
}
finally {
    $fx.Close(); $props.Close()
    Start-Audio
}
# audiodg has restarted without our DLL, so the file is no longer locked.
Remove-Item $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
if (Test-Path $InstallDir) { Write-Warning "$InstallDir still in use; delete it after a reboot." }
if ($RemoveLogs) { Remove-Item $LogDir -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host "`nUninstalled. The original backup stays in $BackupDir."
