# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 RadegasTCZ
<#
.SYNOPSIS
  Downloads the pinned upstream sources needed for AEC3 / AGC2 / RNN VAD into
  <workspace>\upstream (outside the repo). Only the needed directories are fetched,
  as gitiles "+archive" tarballs / a GitHub zip. Re-running skips what is present.
  Revisions match the companion AEC3-Echo-Canceller plugin (tools/aec3-build).
  Afterwards the content of each folder is checked against a pinned SHA-256 (over every file's
  path and content, so it does not depend on how the server packs the archive).
.PARAMETER Root
  Where to put the sources (default: <workspace>\upstream, which cmake\WebrtcUpstream.cmake expects).
.PARAMETER Force
  Download again even if a folder is present.
.PARAMETER SkipVerify
  Do not check the content hashes (only for trying other revisions).
#>
param([string]$Root, [switch]$Force, [switch]$SkipVerify)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$workspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$root = if ($Root) { [IO.Path]::GetFullPath((Join-Path (Get-Location).Path $Root)) } else { Join-Path $workspace 'upstream' }

$webrtcRev   = 'b58dcf8fec3ad5291fb99c7e29650737ae7db953'   # 2026-10-02
$abseilRev   = '972e4ab4a076fca3adbba69b64f2fcdbc78d0e52'   # 2026-03-11
$chromiumRev = '6eabe54fa10922ba0d3f9a8e5a01055d371ebaf7'   # chromium/src/third_party 2026-09-24, as pinned by WebRTC DEPS

# Content hashes of the three folders exactly as fetched by this script (see Get-TreeHash).
$expected = @{
    'webrtc'     = 'c3b82ef01c177d9596dfdeea667b4128aebce5d09e68cd9ffe5444a9f123ac29'
    'chromium'   = '31a496dcb69059c9f0c6c01c495ae6404b72ea1a4e1c749bd0ffa0e118b2ebfc'
    'abseil-cpp' = '617ab02ebb3376781439030a2cb08078fc68f9bcef7800e77f6c5876531285f3'
}

# SHA-256 over every file: ordinally sorted "<relative path>NUL<SHA-256 of the content>" lines.
function Get-TreeHash([string]$dir) {
    $base = (Resolve-Path $dir).Path
    $fileSha = [Security.Cryptography.SHA256]::Create()
    $lines = Get-ChildItem $base -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($base.Length + 1).Replace('\', '/')
        "$rel`0$(-join ($fileSha.ComputeHash([IO.File]::ReadAllBytes($_.FullName)) | ForEach-Object { $_.ToString('X2') }))"
    }
    # Ordinal sort: Sort-Object follows the PC's language settings and the PowerShell version.
    $lines = [string[]]@($lines)
    [Array]::Sort($lines, [StringComparer]::Ordinal)
    $sha = [Security.Cryptography.SHA256]::Create()
    -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))) | ForEach-Object { $_.ToString('x2') })
}

function Get-Archive([string]$url, [string]$dest) {
    if ((Test-Path $dest) -and -not $Force) { Write-Host "  have $dest"; return }
    if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
    # Extract into a new empty folder next to the destination and rename it only when everything worked:
    # a failed download never leaves a folder that the next run would take as complete.
    $part = "$dest.partial"
    if (Test-Path $part) { Remove-Item $part -Recurse -Force }
    New-Item -ItemType Directory -Force $part | Out-Null
    $tmp = Join-Path $env:TEMP ("upstream-" + [guid]::NewGuid() + ".tar.gz")
    Write-Host "  fetch $url"
    try {
        Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing
        & tar.exe -xzf $tmp -C $part
        if ($LASTEXITCODE) { throw "tar failed for $url" }
        Rename-Item $part (Split-Path $dest -Leaf)
    }
    finally { Remove-Item $tmp -Force -EA SilentlyContinue }
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

if (-not $SkipVerify) {
    Write-Host "Checking content hashes"
    $bad = @()
    foreach ($name in $expected.Keys) {
        $got = Get-TreeHash (Join-Path $root $name)
        if ($got -ne $expected[$name]) { $bad += "$name (got $got)" } else { Write-Host "  $name OK" }
    }
    if ($bad) { throw "Upstream content differs from the pinned revision: $($bad -join ', '). Delete the folder(s) and run again with -Force." }
}

Write-Host "`nDone: $root"
