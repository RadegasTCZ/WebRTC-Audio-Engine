<#
.SYNOPSIS
  Downloads the pinned upstream sources needed for AEC3 / AGC2 / RNN VAD into
  <workspace>\upstream (outside the repo). Only the needed directories are fetched,
  as gitiles "+archive" tarballs / a GitHub zip. Re-running skips what is present.
  Revisions match the owner's AEC3-Echo-Canceller plugin (tools/aec3-build).
#>
param([switch]$Force)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$workspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$root = Join-Path $workspace 'upstream'

$webrtcRev   = 'b58dcf8fec3ad5291fb99c7e29650737ae7db953'   # 2026-10-02
$abseilRev   = '972e4ab4a076fca3adbba69b64f2fcdbc78d0e52'   # 2026-03-11
$chromiumRev = '6eabe54fa1'                                 # chromium/src/third_party, as pinned by WebRTC DEPS

function Get-Archive([string]$url, [string]$dest) {
    if ((Test-Path $dest) -and -not $Force) { Write-Host "  have $dest"; return }
    if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
    New-Item -ItemType Directory -Force $dest | Out-Null   # each archive into its own empty directory
    $tmp = Join-Path $env:TEMP ("upstream-" + [guid]::NewGuid() + ".tar.gz")
    Write-Host "  fetch $url"
    Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing
    & tar.exe -xzf $tmp -C $dest
    if ($LASTEXITCODE) { throw "tar failed for $url" }
    Remove-Item $tmp
}

function Get-TextFile([string]$url, [string]$dest) {
    if ((Test-Path $dest) -and -not $Force) { return }
    # gitiles ?format=TEXT returns base64
    $b64 = (Invoke-WebRequest -Uri $url -UseBasicParsing).Content
    [IO.File]::WriteAllBytes($dest, [Convert]::FromBase64String($b64))
}

Write-Host "WebRTC $webrtcRev"
$w = Join-Path $root 'webrtc'
foreach ($dir in 'api', 'rtc_base', 'system_wrappers', 'common_audio', 'modules/audio_processing', 'modules/include', 'experiments') {
    Get-Archive "https://webrtc.googlesource.com/src/+archive/$webrtcRev/$dir.tar.gz" (Join-Path $w $dir)
}
foreach ($f in 'LICENSE', 'PATENTS', 'AUTHORS') {
    Get-TextFile "https://webrtc.googlesource.com/src/+/$webrtcRev/$f`?format=TEXT" (Join-Path $w $f)
}

Write-Host "Chromium third_party $chromiumRev (rnnoise, pffft)"
$c = Join-Path $root 'chromium'
foreach ($dir in 'rnnoise', 'pffft') {
    Get-Archive "https://chromium.googlesource.com/chromium/src/third_party/+archive/$chromiumRev/$dir.tar.gz" (Join-Path $c "third_party\$dir")
}

Write-Host "Abseil $abseilRev"
$a = Join-Path $root 'abseil-cpp'
if (-not (Test-Path $a) -or $Force) {
    if (Test-Path $a) { Remove-Item $a -Recurse -Force }
    $tmpDir = Join-Path $env:TEMP ("abseil-" + [guid]::NewGuid())
    New-Item -ItemType Directory $tmpDir | Out-Null
    $zip = Join-Path $tmpDir 'abseil.zip'
    Invoke-WebRequest -Uri "https://github.com/abseil/abseil-cpp/archive/$abseilRev.zip" -OutFile $zip -UseBasicParsing
    Expand-Archive $zip -DestinationPath $tmpDir
    Move-Item (Join-Path $tmpDir "abseil-cpp-$abseilRev") $a
    Remove-Item $tmpDir -Recurse -Force
} else { Write-Host "  have $a" }

Write-Host "`nDone: $root"
