# Shared settings for the effect-pack package scripts. Dot-source this file.
. (Join-Path $PSScriptRoot 'ApoConfig.ps1')

$Script:RepoRoot        = Split-Path $PSScriptRoot -Parent
$Script:PackageSrc      = Join-Path $RepoRoot 'package'
$Script:PackageOut      = Join-Path $WorkspaceRoot 'build\package'
$Script:CertSubject     = 'CN=WebRTC Audio Engine Test Signing (RadegasTCZ)'
$Script:ComponentInf    = 'WebrtcAudioEngine.inf'
$Script:ExtensionInf    = 'WebrtcAudioEngine-Extension.inf'
$Script:AppInf          = 'WebrtcAudioEngine-App.inf'
$Script:SwcHardwareId   = 'SWC\VEN_WRTC&AUDIO_EFFECTPACK_WEBRTCENGINE'
$Script:AppHardwareId   = 'SWC\VEN_WRTC&WEBRTCENGINE_SETTINGSAPP'
$Script:DefaultAppExe   = Join-Path $WorkspaceRoot 'build\Release\WebrtcAudioEngineSettings.exe'
$Script:EffectPackId    = '{9879C461-0BA4-4A7B-9F35-3C04EAC1772D}'

function Find-KitTool([string]$Name) {
    $t = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Recurse -Filter $Name -EA SilentlyContinue |
         Where-Object { $_.FullName -match '\\(x64|x86)\\' } | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $t) { throw "$Name not found in the Windows Kits" }
    $t.FullName
}

function Get-TestCert {
    Get-ChildItem Cert:\LocalMachine\My | Where-Object Subject -eq $CertSubject | Select-Object -First 1
}

# Published oemNN.inf names of our packages in the driver store.
function Get-OurDriverPackages {
    $out = & pnputil.exe /enum-drivers 2>$null
    $cur = @{}
    $list = @()
    foreach ($line in $out) {
        if ($line -match '^\s*Published Name\s*:\s*(\S+)') { $cur = @{ Published = $Matches[1] } }
        elseif ($line -match '^\s*Original Name\s*:\s*(\S+)') {
            $cur.Original = $Matches[1]
            if ($cur.Original -in @($ComponentInf, $ExtensionInf, $AppInf)) { $list += [pscustomobject]$cur }  # -in ignores case
        }
    }
    $list
}
