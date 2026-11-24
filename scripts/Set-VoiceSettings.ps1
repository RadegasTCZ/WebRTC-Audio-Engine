# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 RadegasTCZ
<#
.SYNOPSIS
  Shows or changes the voice-processing settings. Changes apply LIVE to running recordings.
  Run once as admin to create the settings key; after that, Users may change values.
.EXAMPLE
  .\Set-VoiceSettings.ps1                          # show current settings
  .\Set-VoiceSettings.ps1 -Bypass on               # untouched audio (same delay, volume slider kept)
  .\Set-VoiceSettings.ps1 -Cleanup both            # off | rumble (high-pass) | hiss (low-pass) | both
  .\Set-VoiceSettings.ps1 -RumbleLevel strong      # gentle 60 Hz 12 dB/oct | normal 80 Hz 12 dB/oct | strong 120 Hz 18 dB/oct
  .\Set-VoiceSettings.ps1 -HissLevel normal        # gentle 10 kHz 6 dB/oct | normal 9 kHz 12 dB/oct | strong 8 kHz 18 dB/oct
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
    [ValidateSet('gentle', 'normal', 'strong')][string]$RumbleLevel,
    [ValidateSet('gentle', 'normal', 'strong')][string]$HissLevel,
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
    [ValidateSet('gentle', 'normal', 'strong')][string]$AecStrength,
    [ValidateSet('small', 'medium', 'large')][string]$AecRoom
)
$ErrorActionPreference = 'Stop'
$path = 'SOFTWARE\WebrtcVoiceApo'
$onOff = { param($v) if ($v -eq 'on') { 1 } else { 0 } }
# AGC presets from the AEC3-Echo-Canceller plugin (headroom, max gain, speed, noise ceiling).
$presets = @{ gentle = @(8, 20, 3, -60); normal = @(5, 30, 6, -50); strong = @(3, 50, 10, -45) }

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$k = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($path, $true)
if (-not $k) {
    # Create by full path (write-opening HKLM\SOFTWARE itself is refused).
    $k = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey($path)
    Write-Host "Created HKLM\$path."
}
if ($isAdmin) {
    # Same ACLs as the installer (app\Setup.cpp PrepareSettingsKey): Users, the audio engine account and app
    # containers may change values; under Devices they may also add the per-microphone subkeys. Set with
    # RegSetKeySecurity, on each key only: .NET's SetAccessControl would also rewrite every subkey.
    Add-Type -Namespace WebrtcAudioEngine -Name Native -MemberDefinition @'
[DllImport("advapi32.dll")] public static extern int RegSetKeySecurity(Microsoft.Win32.SafeHandles.SafeRegistryHandle key, int info, byte[] sd);
'@
    $setSddl = {
        param($key, $sddl)
        $sd = [System.Security.AccessControl.RawSecurityDescriptor]::new($sddl)
        $bytes = [byte[]]::new($sd.BinaryLength); $sd.GetBinaryForm($bytes, 0)
        $e = [WebrtcAudioEngine.Native]::RegSetKeySecurity($key.Handle, 0x5, $bytes)   # OWNER + DACL
        if ($e) { Write-Warning "ACL not set on $($key.Name) (error $e)" }
    }
    $full = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($path, 'ReadWriteSubTree', 'FullControl')
    & $setSddl $full 'O:BAD:P(A;OICI;KA;;;SY)(A;OICI;KA;;;BA)(A;;0x2001b;;;BU)(A;;0x2001b;;;LS)(A;;0x2001b;;;AC)(A;;0x2001b;;;S-1-15-2-2)'
    $full.CreateSubKey('Devices').Close()
    $dev = $full.OpenSubKey('Devices', 'ReadWriteSubTree', 'FullControl')
    & $setSddl $dev 'O:BAD:P(A;OICI;KA;;;SY)(A;OICI;KA;;;BA)(A;OICI;0x2001f;;;BU)(A;OICI;0x2001f;;;LS)(A;OICI;0x2001f;;;AC)(A;OICI;0x2001f;;;S-1-15-2-2)(A;OICI;0x2001f;;;OW)'
    $dev.Close(); $full.Close()
}
$dw = { param($name, $val) $k.SetValue($name, [int]$val, 'DWord') }
if ($PSBoundParameters.ContainsKey('Bypass'))  { & $dw 'Bypass' (& $onOff $Bypass) }
if ($Cleanup) { & $dw 'CleanupFilters' ([array]::IndexOf(@('off', 'rumble', 'hiss', 'both'), $Cleanup)) }
$levels = @('gentle', 'normal', 'strong')
if ($RumbleLevel) { & $dw 'RumbleLevel' ([array]::IndexOf($levels, $RumbleLevel)) }
if ($HissLevel)   { & $dw 'HissLevel' ([array]::IndexOf($levels, $HissLevel)) }
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
if ($AecRoom) { & $dw 'AecRoom' ([array]::IndexOf(@('small', 'medium', 'large'), $AecRoom)) }

$get = { param($n, $d) $v = $k.GetValue($n); if ($null -eq $v) { $d } else { [int]$v } }
$st = { param($v) if ($v) { 'on' } else { 'off' } }
$cur = @((& $get 'AgcHeadroomDb' 5), (& $get 'AgcMaxGainDb' 30), (& $get 'AgcSpeedDbPerSec' 6), (& $get 'AgcNoiseCeilingDbfs' -50))
$presetName = ($presets.Keys | Where-Object { (Compare-Object $presets[$_] $cur) -eq $null } | Select-Object -First 1) ?? 'custom'
$cl = & $get 'CleanupFilters' -1; if ($cl -lt 0) { $cl = 2 + (& $get 'HpfEnabled' 1) }   # default: hiss on + rumble
Write-Host ("Bypass {0} | AEC {13} ({14}, {17} room) | Cleanup {15} (rumble {2}, hiss {16}) | NS {3} @ {4} dB | AGC {5} ({6}: headroom {7}, max {8}, speed {9}, noise {10}) | Limiter {11} | Slider gain {12}" -f `
    (& $st (& $get 'Bypass' 0)), '', $levels[(& $get 'RumbleLevel' 1)], (& $st (& $get 'NsEnabled' 1)),
    @(6, 12, 18, 21)[(& $get 'NsLevel' 1)], (& $st (& $get 'AgcEnabled' 0)), $presetName, $cur[0], $cur[1], $cur[2], $cur[3],
    (& $st (& $get 'LimiterEnabled' 1)), @('auto', 'always', 'never')[(& $get 'GainMode' 0)],
    (& $st (& $get 'AecEnabled' 1)), @('gentle', 'normal', 'strong')[(& $get 'AecStrength' 1)],
    @('off', 'rumble', 'hiss', 'both')[$cl], $levels[(& $get 'HissLevel' 0)], @('small', 'medium', 'large')[(& $get 'AecRoom' 0)])
$k.Close()
