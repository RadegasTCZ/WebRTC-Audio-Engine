<#
.SYNOPSIS
  Shows or changes the voice-processing settings. Changes apply LIVE to running recordings.
  Run once as admin to create the settings key; after that, Users may change values.
.EXAMPLE
  .\Set-VoiceSettings.ps1                          # show current settings
  .\Set-VoiceSettings.ps1 -Bypass on               # untouched audio (same delay, volume slider kept)
  .\Set-VoiceSettings.ps1 -Cleanup both            # off | rumble (80 Hz high-pass) | hiss (10 kHz low-pass, 6 dB/oct) | both
  .\Set-VoiceSettings.ps1 -HpfHz 100 -LpfHz 12000
  .\Set-VoiceSettings.ps1 -Ns on -NsLevel 18       # 6, 12, 18 or 21 dB
  .\Set-VoiceSettings.ps1 -AgcPreset normal        # gentle | normal | strong (also switches AGC on)
  .\Set-VoiceSettings.ps1 -Agc off
  .\Set-VoiceSettings.ps1 -AgcMaxGain 40 -AgcHeadroom 4 -AgcSpeed 8 -AgcNoiseCeiling -55
  .\Set-VoiceSettings.ps1 -Limiter on
  .\Set-VoiceSettings.ps1 -GainMode auto           # mic slider in software: auto | always | never
#>
param(
    [ValidateSet('on', 'off')][string]$Bypass,
    [ValidateSet('off', 'rumble', 'hiss', 'both')][string]$Cleanup,
    [ValidateRange(8000, 16000)][int]$LpfHz,
    [ValidateRange(20, 300)][int]$HpfHz,
    [ValidateSet('on', 'off')][string]$Ns,
    [ValidateSet(6, 12, 18, 21)][int]$NsLevel,
    [ValidateSet('on', 'off')][string]$Agc,
    [ValidateSet('gentle', 'normal', 'strong')][string]$AgcPreset,
    [ValidateRange(0, 20)][int]$AgcHeadroom,
    [ValidateRange(1, 50)][int]$AgcMaxGain,
    [ValidateRange(1, 30)][int]$AgcSpeed,
    [ValidateRange(-80, -20)][int]$AgcNoiseCeiling,
    [ValidateSet('on', 'off')][string]$Limiter,
    [ValidateSet('auto', 'always', 'never')][string]$GainMode,
    [ValidateSet('on', 'off')][string]$Aec,
    [ValidateSet('gentle', 'normal', 'strong')][string]$AecStrength
)
$ErrorActionPreference = 'Stop'
$path = 'SOFTWARE\WebrtcVoiceApo'
$onOff = { param($v) if ($v -eq 'on') { 1 } else { 0 } }
# AGC presets from the AEC3-Echo-Canceller plugin (headroom, max gain, speed, noise ceiling).
$presets = @{ gentle = @(8, 20, 3, -60); normal = @(5, 30, 6, -50); strong = @(3, 50, 10, -45) }

$k = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($path, $true)
if (-not $k) {
    # Create by full path (write-opening HKLM\SOFTWARE itself is refused) and let Users change values.
    $k = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey($path)
    $acl = $k.GetAccessControl()
    $acl.AddAccessRule([System.Security.AccessControl.RegistryAccessRule]::new(
        [System.Security.Principal.SecurityIdentifier]::new('S-1-5-32-545'),   # BUILTIN\Users
        [System.Security.AccessControl.RegistryRights]'QueryValues, SetValue, Notify, ReadKey',
        [System.Security.AccessControl.AccessControlType]::Allow))
    $k.SetAccessControl($acl)
    Write-Host "Created HKLM\$path (Users may change values)."
}
$dw = { param($name, $val) $k.SetValue($name, [int]$val, 'DWord') }
if ($PSBoundParameters.ContainsKey('Bypass'))  { & $dw 'Bypass' (& $onOff $Bypass) }
if ($Cleanup) { & $dw 'CleanupFilters' ([array]::IndexOf(@('off', 'rumble', 'hiss', 'both'), $Cleanup)) }
if ($PSBoundParameters.ContainsKey('LpfHz')) { & $dw 'LpfHz' $LpfHz }
if ($PSBoundParameters.ContainsKey('HpfHz'))   { & $dw 'HpfHz' $HpfHz }
if ($PSBoundParameters.ContainsKey('Ns'))      { & $dw 'NsEnabled' (& $onOff $Ns) }
if ($PSBoundParameters.ContainsKey('NsLevel')) { & $dw 'NsLevel' ([array]::IndexOf(@(6, 12, 18, 21), $NsLevel)) }
if ($AgcPreset) {
    $p = $presets[$AgcPreset]
    & $dw 'AgcHeadroomDb' $p[0]; & $dw 'AgcMaxGainDb' $p[1]; & $dw 'AgcSpeedDbPerSec' $p[2]; & $dw 'AgcNoiseCeilingDbfs' $p[3]
    & $dw 'AgcEnabled' 1
}
if ($PSBoundParameters.ContainsKey('Agc'))             { & $dw 'AgcEnabled' (& $onOff $Agc) }
if ($PSBoundParameters.ContainsKey('AgcHeadroom'))     { & $dw 'AgcHeadroomDb' $AgcHeadroom }
if ($PSBoundParameters.ContainsKey('AgcMaxGain'))      { & $dw 'AgcMaxGainDb' $AgcMaxGain }
if ($PSBoundParameters.ContainsKey('AgcSpeed'))        { & $dw 'AgcSpeedDbPerSec' $AgcSpeed }
if ($PSBoundParameters.ContainsKey('AgcNoiseCeiling')) { & $dw 'AgcNoiseCeilingDbfs' $AgcNoiseCeiling }
if ($PSBoundParameters.ContainsKey('Limiter'))         { & $dw 'LimiterEnabled' (& $onOff $Limiter) }
if ($GainMode) { & $dw 'GainMode' ([array]::IndexOf(@('auto', 'always', 'never'), $GainMode)) }
if ($PSBoundParameters.ContainsKey('Aec')) { & $dw 'AecEnabled' (& $onOff $Aec) }
if ($AecStrength) { & $dw 'AecStrength' ([array]::IndexOf(@('gentle', 'normal', 'strong'), $AecStrength)) }

$get = { param($n, $d) $v = $k.GetValue($n); if ($null -eq $v) { $d } else { [int]$v } }
$st = { param($v) if ($v) { 'on' } else { 'off' } }
$cur = @((& $get 'AgcHeadroomDb' 5), (& $get 'AgcMaxGainDb' 30), (& $get 'AgcSpeedDbPerSec' 6), (& $get 'AgcNoiseCeilingDbfs' -50))
$presetName = ($presets.Keys | Where-Object { (Compare-Object $presets[$_] $cur) -eq $null } | Select-Object -First 1) ?? 'custom'
$cl = & $get 'CleanupFilters' -1; if ($cl -lt 0) { $cl = 2 + (& $get 'HpfEnabled' 1) }   # default: hiss on + rumble
Write-Host ("Bypass {0} | AEC {13} ({14}) | Cleanup {15} (rumble {2} Hz, hiss {16} Hz) | NS {3} @ {4} dB | AGC {5} ({6}: headroom {7}, max {8}, speed {9}, noise {10}) | Limiter {11} | Slider gain {12}" -f `
    (& $st (& $get 'Bypass' 0)), '', (& $get 'HpfHz' 80), (& $st (& $get 'NsEnabled' 1)),
    @(6, 12, 18, 21)[(& $get 'NsLevel' 1)], (& $st (& $get 'AgcEnabled' 0)), $presetName, $cur[0], $cur[1], $cur[2], $cur[3],
    (& $st (& $get 'LimiterEnabled' 1)), @('auto', 'always', 'never')[(& $get 'GainMode' 0)],
    (& $st (& $get 'AecEnabled' 1)), @('gentle', 'normal', 'strong')[(& $get 'AecStrength' 1)],
    @('off', 'rumble', 'hiss', 'both')[$cl], (& $get 'LpfHz' 10000))
$k.Close()
