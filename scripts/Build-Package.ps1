# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 RadegasTCZ
<#
.SYNOPSIS
  Stages the driver packages (effect pack, settings app, extension) into <workspace>\build\package,
  creates the catalogs with Inf2Cat and test-signs DLL, exe and catalogs. Changes nothing on the system.
#>
param([string]$DllPath, [string]$AppExe)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageConfig.ps1')
if (-not $DllPath) { $DllPath = $DefaultDll }
if (-not (Test-Path $DllPath)) { throw "DLL not found: $DllPath (build first)" }
if (-not $AppExe) { $AppExe = $DefaultAppExe }
if (-not (Test-Path $AppExe)) { throw "Settings app not found: $AppExe (build first)" }
$cert = Get-TestCert
if (-not $cert) { throw 'No test certificate. Run .\New-TestCert.ps1 once (admin).' }
$inf2cat = Find-KitTool 'Inf2Cat.exe'
$signtool = Find-KitTool 'signtool.exe'

# Windows keeps an installed package whose DriverVer did not change ("already exists"), even if the files
# are new. Every staged INF gets today's date and a version that grows with every build: major.minor from
# the source INF, then days since 2026-01-01 and 2-second steps since midnight (each field below 65536),
# in local standard time (summer time ignored) so the number never goes back when the clocks change.
# The app INF's SoftwareVersion follows it too: Windows runs the app's installer once per SoftwareVersion.
$now = [datetime]::UtcNow + [TimeZoneInfo]::Local.BaseUtcOffset
$build = '{0}.{1}' -f [int]($now.Date - [datetime]'2026-01-01').TotalDays, [int][math]::Floor($now.TimeOfDay.TotalSeconds / 2)
$payload = @{ component = $DllPath; app = $AppExe }

# The binaries carry the same version as the packages (Explorer > Properties > Details): write it into the
# build's version.h (src/VersionInfo.rc.inc) and rebuild them. major.minor from the component INF.
$buildDir = Join-Path $WorkspaceRoot 'build'
if ($DllPath -eq $DefaultDll -and $AppExe -eq $DefaultAppExe -and (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
    $srcInf = [IO.File]::ReadAllText((Join-Path $PackageSrc "component\$ComponentInf"))
    if ($srcInf -notmatch 'DriverVer\s*=\s*[^,\r\n]+,\s*(\d+)\.(\d+)\.\d+\.\d+') { throw "No DriverVer in $ComponentInf" }
    $fileVersion = '{0}.{1}.{2}' -f $Matches[1], $Matches[2], $build
    $versionH = Join-Path $buildDir 'generated\version.h'
    New-Item -ItemType Directory -Force (Split-Path $versionH) | Out-Null
    [IO.File]::WriteAllText($versionH, (@(
        '// Written by scripts/Build-Package.ps1: the version of the packages built from these binaries.',
        ('#define WAE_VERSION_BIN   {0}' -f ($fileVersion -replace '\.', ',')),
        ('#define WAE_VERSION_STR   "{0}"' -f $fileVersion),
        '#define WAE_VERSION_FLAGS 0', '') -join "`r`n"))
    Write-Host "Rebuilding the binaries as version $fileVersion..."
    $o = & cmake --build $buildDir --config Release --target WebrtcVoiceApo WebrtcAudioEngineSettings SetupActions 2>&1
    if ($LASTEXITCODE) { $o | Write-Host; throw 'Rebuilding with the version resource failed' }
} else {
    Write-Warning 'Custom binaries or no CMake build: their version resource is left as built.'
}

foreach ($part in 'component', 'app', 'extension') {
    $dst = Join-Path $PackageOut $part
    if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
    New-Item -ItemType Directory -Force $dst | Out-Null
    Copy-Item (Join-Path $PackageSrc "$part\*.inf") $dst
    $inf = Get-ChildItem $dst -Filter *.inf | Select-Object -First 1
    $text = [IO.File]::ReadAllText($inf.FullName)
    if ($text -notmatch 'DriverVer\s*=\s*[^,\r\n]+,\s*(\d+)\.(\d+)\.\d+\.\d+') { throw "No DriverVer in $($inf.Name)" }
    $version = '{0}.{1}.{2}' -f $Matches[1], $Matches[2], $build
    $text = $text -replace 'DriverVer\s*=\s*[^\r\n]+', ('DriverVer   = {0},{1}' -f $now.ToString('MM/dd/yyyy', [cultureinfo]::InvariantCulture), $version)
    $text = $text -replace '(?m)^(SoftwareVersion\s*=\s*)\S+', "`${1}$version"
    $text = $text -replace '(?m)^(SoftwareArguments\s*=\s*/install)\s+\S+', "`${1} $version"
    [IO.File]::WriteAllText($inf.FullName, $text)
    Write-Host "  $part version $version"
    if ($payload[$part]) {
        Copy-Item $payload[$part] $dst
        & $signtool sign /q /sm /s My /sha1 $cert.Thumbprint /fd sha256 (Join-Path $dst (Split-Path $payload[$part] -Leaf))
        if ($LASTEXITCODE) { throw "signtool failed on $(Split-Path $payload[$part] -Leaf)" }
    }
    Write-Host "Inf2Cat ($part)..."
    $o = & $inf2cat /driver:$dst /os:10_GE_X64 /uselocaltime 2>&1
    if ($LASTEXITCODE) { $o | Write-Host; throw "Inf2Cat failed for $part" }
    Get-ChildItem $dst -Filter *.cat | ForEach-Object {
        & $signtool sign /q /sm /s My /sha1 $cert.Thumbprint /fd sha256 $_.FullName
        if ($LASTEXITCODE) { throw "signtool failed on $($_.Name)" }
    }
    Write-Host "  $part ready: $((Get-ChildItem $dst | ForEach-Object Name) -join ', ')"
}
Write-Host "`nPackages in $PackageOut"
