<#
.SYNOPSIS
  Installs the "WebRTC Audio Engine" effect pack (component, settings app and extension driver packages)
  with pnputil. Windows Audio is stopped while the packages are installed and started again afterwards.
  Windows then installs the settings app by itself (Program Files, Start menu, tray icon); its log is
  %ProgramData%\WebrtcAudioEngine\setup.log.
  Undo with .\Uninstall-Package.ps1.
#>
param([switch]$DryRun)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageConfig.ps1')
Assert-Admin
$comp = Join-Path $PackageOut "component\$ComponentInf"
$app  = Join-Path $PackageOut "app\$AppInf"
$ext  = Join-Path $PackageOut "extension\$ExtensionInf"
foreach ($f in $comp, $app, $ext) { if (-not (Test-Path $f)) { throw "Missing $f (run .\Build-Package.ps1)" } }

Write-Host "Will install:`n  $comp`n  $app`n  $ext"
$existing = Get-OurDriverPackages
if ($existing) { Write-Host "Already in the driver store: $(($existing | ForEach-Object { "$($_.Published) ($($_.Original))" }) -join ', ')" }
if ($DryRun) { Write-Host "`nDry run: nothing was changed."; return }

Stop-Audio
try {
    # Components first, so they are in the store when the extension creates the software components.
    & pnputil.exe /add-driver $comp /install | Write-Host
    & pnputil.exe /add-driver $app /install | Write-Host
    & pnputil.exe /add-driver $ext /install | Write-Host
}
finally { Start-Audio }

# Remove superseded versions of each package (the devices now use the new ones). No /force: pnputil
# refuses a package that is still in use.
$current = Get-OurDriverPackages
foreach ($inf in $ComponentInf, $AppInf, $ExtensionInf) {
    $ourNew = ($current | Where-Object { $_.Published -notin $existing.Published -and $_.Original -ieq $inf }).Published
    if (-not $ourNew) { Write-Host "No new $inf was added (same version already installed?): nothing to clean up."; continue }
    foreach ($old in ($existing | Where-Object { $_.Original -ieq $inf -and $_.Published -ne $ourNew })) {
        $o = & pnputil.exe /delete-driver $old.Published 2>&1 | Out-String
        Write-Host "Old package $($old.Published) ($inf): $(if ($o -match 'deleted successfully') { 'removed' } else { 'kept (' + (($o -split "`n" | Where-Object { $_ -match '\S' } | Select-Object -Last 1).Trim()) + ')' })"
    }
}

Start-Sleep 2
Write-Host "`n--- result"
Get-PnpDevice -PresentOnly | Where-Object { $_.HardwareID -contains $SwcHardwareId -or $_.HardwareID -contains $AppHardwareId } |
    ForEach-Object { Write-Host "Device: $($_.Status) $($_.Class) '$($_.FriendlyName)' $($_.InstanceId)" }
$reg = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\EffectsPacks\$EffectPackId"
Write-Host "Registered with the audio system (MMDevices\EffectsPacks): $(Test-Path $reg)"
Write-Host "Settings app: $(if (Test-Path $InstalledAppExe) { (Get-Item $InstalledAppExe).LastWriteTime.ToString('g') + ' ' + $InstalledAppExe } else { "not installed yet (see $SetupLog)" })"
Write-Host 'Now check Settings > System > Sound > (microphone) > Audio enhancements.'
