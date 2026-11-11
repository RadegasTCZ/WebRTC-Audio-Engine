<#
.SYNOPSIS
  Shows or changes the voice-processing settings. Changes apply LIVE (within ~10 ms) to running
  recordings. Run once as admin to create the settings key; after that, Users may change values.
.EXAMPLE
  .\Set-VoiceSettings.ps1                          # show current settings
  .\Set-VoiceSettings.ps1 -Bypass on               # untouched audio (same delay), for A/B listening
  .\Set-VoiceSettings.ps1 -Hpf off -HpfHz 100
  .\Set-VoiceSettings.ps1 -Ns on -NsLevel 18       # 6, 12, 18 or 21 dB
#>
param(
    [ValidateSet('on', 'off')][string]$Bypass,
    [ValidateSet('on', 'off')][string]$Hpf,
    [ValidateRange(20, 300)][int]$HpfHz,
    [ValidateSet('on', 'off')][string]$Ns,
    [ValidateSet(6, 12, 18, 21)][int]$NsLevel
)
$ErrorActionPreference = 'Stop'
$path = 'SOFTWARE\WebrtcVoiceApo'
$onOff = { param($v) if ($v -eq 'on') { 1 } else { 0 } }

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
if ($PSBoundParameters.ContainsKey('Bypass'))  { $k.SetValue('Bypass', (& $onOff $Bypass), 'DWord') }
if ($PSBoundParameters.ContainsKey('Hpf'))     { $k.SetValue('HpfEnabled', (& $onOff $Hpf), 'DWord') }
if ($PSBoundParameters.ContainsKey('HpfHz'))   { $k.SetValue('HpfHz', $HpfHz, 'DWord') }
if ($PSBoundParameters.ContainsKey('Ns'))      { $k.SetValue('NsEnabled', (& $onOff $Ns), 'DWord') }
if ($PSBoundParameters.ContainsKey('NsLevel')) { $k.SetValue('NsLevel', [array]::IndexOf(@(6, 12, 18, 21), $NsLevel), 'DWord') }

$get = { param($n, $d) $v = $k.GetValue($n); if ($null -eq $v) { $d } else { $v } }
$st = { param($v) if ($v) { 'on' } else { 'off' } }
Write-Host ("Bypass {0} | HPF {1} @ {2} Hz | NS {3} @ {4} dB" -f (& $st (& $get 'Bypass' 0)), (& $st (& $get 'HpfEnabled' 1)),
    (& $get 'HpfHz' 80), (& $st (& $get 'NsEnabled' 1)), @(6, 12, 18, 21)[(& $get 'NsLevel' 1)])
$k.Close()
