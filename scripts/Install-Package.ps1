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

# Remove superseded versions of the component package (the device now uses the new one). No /force:
# pnputil refuses a package that is still in use.
$ourNew = (Get-OurDriverPackages | Where-Object { $_.Published -notin $existing.Published -and $_.Original -ieq $ComponentInf }).Published
if (-not $ourNew) { Write-Host 'No new component package was added (same version already installed?): nothing to clean up.' }
else { foreach ($old in ($existing | Where-Object { $_.Original -ieq $ComponentInf -and $_.Published -ne $ourNew })) {
    $o = & pnputil.exe /delete-driver $old.Published 2>&1 | Out-String
    Write-Host "Old package $($old.Published): $(if ($o -match 'deleted successfully') { 'removed' } else { 'kept (' + (($o -split "`n" | Where-Object { $_ -match '\S' } | Select-Object -Last 1).Trim()) + ')' })"
} }

Start-Sleep 2
Write-Host "`n--- result"
Get-PnpDevice -PresentOnly | Where-Object { $_.HardwareID -contains $SwcHardwareId } |
    ForEach-Object { Write-Host "Device: $($_.Status) $($_.Class) '$($_.FriendlyName)' $($_.InstanceId)" }
$reg = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\EffectsPacks\$EffectPackId"
Write-Host "Registered with the audio system (MMDevices\EffectsPacks): $(Test-Path $reg)"
Write-Host 'Now check Settings > System > Sound > (microphone) > Audio enhancements.'
