<#
.SYNOPSIS
  Stages the effect-pack driver packages into <workspace>\build\package, creates the catalogs
  with Inf2Cat and test-signs DLL + catalogs. Changes nothing on the system.
#>
param([string]$DllPath)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageConfig.ps1')
if (-not $DllPath) { $DllPath = $DefaultDll }
if (-not (Test-Path $DllPath)) { throw "DLL not found: $DllPath (build first)" }
$cert = Get-TestCert
if (-not $cert) { throw 'No test certificate. Run .\New-TestCert.ps1 once (admin).' }
$inf2cat = Find-KitTool 'Inf2Cat.exe'
$signtool = Find-KitTool 'signtool.exe'

foreach ($part in 'component', 'extension') {
    $dst = Join-Path $PackageOut $part
    if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
    New-Item -ItemType Directory -Force $dst | Out-Null
    Copy-Item (Join-Path $PackageSrc "$part\*.inf") $dst
    if ($part -eq 'component') {
        # Windows keeps an installed package whose DriverVer did not change ("already exists"), even if the
        # DLL is new. Stamp the staged INF with today's date and a version that grows with every build:
        # major.minor from the source INF, then days since 2026-01-01 and 2-second steps since midnight
        # (each field must stay below 65536).
        $inf = Join-Path $dst $ComponentInf
        $text = [IO.File]::ReadAllText($inf)
        if ($text -notmatch 'DriverVer\s*=\s*[^,\r\n]+,\s*(\d+)\.(\d+)\.\d+\.\d+') { throw "No DriverVer in $inf" }
        $now = Get-Date
        $version = '{0}.{1}.{2}.{3}' -f $Matches[1], $Matches[2], [int]($now.Date - [datetime]'2026-01-01').TotalDays,
                   [int][math]::Floor($now.TimeOfDay.TotalSeconds / 2)
        $text = $text -replace 'DriverVer\s*=\s*[^\r\n]+', ('DriverVer   = {0},{1}' -f $now.ToString('MM/dd/yyyy', [cultureinfo]::InvariantCulture), $version)
        [IO.File]::WriteAllText($inf, $text)
        Write-Host "  component version $version"
        Copy-Item $DllPath $dst
        & $signtool sign /q /sm /s My /sha1 $cert.Thumbprint /fd sha256 (Join-Path $dst 'WebrtcVoiceApo.dll')
        if ($LASTEXITCODE) { throw 'signtool failed on the DLL' }
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
