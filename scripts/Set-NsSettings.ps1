<#
.SYNOPSIS
  Changes the noise-suppression settings (admin). They apply the next time an app starts
  recording (stop/start recording, or reopen the app). No audio restart needed.
.EXAMPLE
  .\Set-NsSettings.ps1 -Level 12        # 6, 12, 18 or 21 dB of suppression
  .\Set-NsSettings.ps1 -Bypass on       # untouched audio, for A/B listening
  .\Set-NsSettings.ps1 -Bypass off
  .\Set-NsSettings.ps1                  # show current settings
#>
param(
    [ValidateSet(6, 12, 18, 21)][int]$Level,
    [ValidateSet('on', 'off')][string]$Bypass
)
$ErrorActionPreference = 'Stop'
$key = 'HKLM:\SOFTWARE\WebrtcVoiceApo'
if ($PSBoundParameters.Count -gt 0) {
    # Create by full path: write-opening the parent HKLM\SOFTWARE is refused on this Windows build.
    $k = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey('SOFTWARE\WebrtcVoiceApo')
    if ($PSBoundParameters.ContainsKey('Level'))  { $k.SetValue('NsLevel', [array]::IndexOf(@(6, 12, 18, 21), $Level), 'DWord') }
    if ($PSBoundParameters.ContainsKey('Bypass')) { $k.SetValue('Bypass', $(if ($Bypass -eq 'on') { 1 } else { 0 }), 'DWord') }
    $k.Close()
}

$s = Get-ItemProperty $key -EA SilentlyContinue
$lvl = if ($s -and $null -ne $s.NsLevel) { @(6, 12, 18, 21)[$s.NsLevel] } else { 12 }
$byp = if ($s -and $s.Bypass) { 'on' } else { 'off' }
Write-Host "Noise suppression: $lvl dB, bypass $byp  (applies when an app next starts recording)"
