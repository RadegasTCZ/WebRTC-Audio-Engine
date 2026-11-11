<#
.SYNOPSIS
  Removes the "WebRTC Audio Engine" effect pack: its software-component device and both driver
  packages. Windows Audio is stopped meanwhile. Does not touch the registry-route EFX install.
  -RemoveCert also removes the test certificate.
#>
param([switch]$RemoveCert, [switch]$DryRun)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageConfig.ps1')
Assert-Admin

$devices = @(Get-PnpDevice | Where-Object { $_.HardwareID -contains $SwcHardwareId })
$packages = @(Get-OurDriverPackages)
Write-Host "Devices : $(if ($devices) { ($devices.InstanceId) -join ', ' } else { '(none)' })"
Write-Host "Packages: $(if ($packages) { ($packages | ForEach-Object { "$($_.Published) ($($_.Original))" }) -join ', ' } else { '(none)' })"
if ($DryRun) { Write-Host "`nDry run: nothing was changed."; return }

Stop-Audio
try {
    # Extension first (stops re-creating the component), then the component.
    foreach ($p in ($packages | Sort-Object { $_.Original -ne $ExtensionInf.ToLower() -and $_.Original -ne $ExtensionInf })) {
        & pnputil.exe /delete-driver $p.Published /uninstall /force | Write-Host
    }
    foreach ($d in $devices) { & pnputil.exe /remove-device $d.InstanceId | Write-Host }
}
finally { Start-Audio }

if ($RemoveCert) { & (Join-Path $PSScriptRoot 'New-TestCert.ps1') -Remove }
Write-Host "`nEffect pack removed."
