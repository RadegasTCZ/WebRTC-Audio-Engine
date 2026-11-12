<#
.SYNOPSIS
  Installs the "WebRTC Audio Engine" effect pack (component + extension driver packages) with pnputil.
  Windows Audio is stopped while the packages are installed and started again afterwards.
  Undo with .\Uninstall-Package.ps1.
#>
param([switch]$DryRun)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageConfig.ps1')
Assert-Admin
$comp = Join-Path $PackageOut "component\$ComponentInf"
$ext  = Join-Path $PackageOut "extension\$ExtensionInf"
foreach ($f in $comp, $ext) { if (-not (Test-Path $f)) { throw "Missing $f (run .\Build-Package.ps1)" } }

Write-Host "Will install:`n  $comp`n  $ext"
$existing = Get-OurDriverPackages
if ($existing) { Write-Host "Already in the driver store: $(($existing | ForEach-Object { "$($_.Published) ($($_.Original))" }) -join ', ')" }
if ($DryRun) { Write-Host "`nDry run: nothing was changed."; return }

Initialize-LogDir  # diagnostics log of the effect (writable only by the audio engine)
Stop-Audio
try {
    # Component first, so it is in the store when the extension creates the software component.
    & pnputil.exe /add-driver $comp /install | Write-Host
    & pnputil.exe /add-driver $ext /install | Write-Host
}
finally { Start-Audio }

Start-Sleep 2
Write-Host "`n--- result"
Get-PnpDevice -PresentOnly | Where-Object { $_.HardwareID -contains $SwcHardwareId } |
    ForEach-Object { Write-Host "Device: $($_.Status) $($_.Class) '$($_.FriendlyName)' $($_.InstanceId)" }
$reg = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\EffectsPacks\$EffectPackId"
Write-Host "Registered with the audio system (MMDevices\EffectsPacks): $(Test-Path $reg)"
Write-Host 'Now check Settings > System > Sound > (microphone) > Audio enhancements.'
