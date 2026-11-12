<#
.SYNOPSIS
  Checks that the effect DLL runs on any x64 CPU: every AVX/AVX2/AVX-512 instruction must sit behind a CPU
  check. Allowed: the C/C++ runtime (it checks the CPU itself), our *_avx2 kernels (WebRTC calls them only
  when cpu_info detects AVX2), and functions where the compiler added its own check (__isa_available).
  Anything else, e.g. a helper whose AVX2 copy the linker picked, is reported and the script fails.
  Needs the linker map (the build writes ..\build\Release\WebrtcVoiceApo.map) and dumpbin from Visual Studio.
#>
param([string]$Dll)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ApoConfig.ps1')
if (-not $Dll) { $Dll = $DefaultDll }
$map = [IO.Path]::ChangeExtension($Dll, '.map')
if (-not (Test-Path $map)) { throw "No linker map next to $Dll (rebuild: the DLL links with /MAP)" }
$dumpbin = Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" |
           Sort-Object FullName -Descending | Select-Object -First 1
if (-not $dumpbin) { throw 'dumpbin.exe not found (Visual Studio C++ tools)' }

# Function start addresses -> name and object file.
$syms = [Collections.Generic.List[object]]::new()
foreach ($line in [IO.File]::ReadLines($map)) {
    if ($line -match '^\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+(?:f\s+)?(?:i\s+)?(\S+)\s*$') {
        $syms.Add([pscustomobject]@{ Addr = [Convert]::ToUInt64($Matches[2], 16); Name = $Matches[1]; Obj = $Matches[3] })
    }
}
$syms = $syms | Sort-Object Addr
$addrs = [uint64[]]($syms | ForEach-Object Addr)
$isa = ($syms | Where-Object Name -eq '__isa_available').Addr
if (-not $isa) { throw '__isa_available not found in the map' }
$isaText = '{0:X16}h' -f $isa

$dis = Join-Path $env:TEMP 'WebrtcVoiceApo.disasm.txt'
& $dumpbin.FullName /nologo /disasm:nobytes /section:.text "/out:$dis" $Dll | Out-Null

$avx = @{}; $guarded = @{}
foreach ($line in [IO.File]::ReadLines($dis)) {
    if ($line -notmatch '^\s+([0-9A-F]{16}):\s+(\S+)\s*(.*)$') { continue }
    $addr = [Convert]::ToUInt64($Matches[1], 16); $ins = $Matches[2]; $ops = $Matches[3]
    $i = [Array]::BinarySearch($addrs, $addr); if ($i -lt 0) { $i = (-bnot $i) - 1 }; if ($i -lt 0) { continue }
    $key = $syms[$i].Obj + '|' + $syms[$i].Name
    if ($ops -like "*$isaText*") { $guarded[$key] = $true }
    if (($ins.StartsWith('v') -and $ops -match 'mm') -or $ops -match 'ymm|zmm') { $avx[$key] = 1 + [int]$avx[$key] }
}

$bad = foreach ($key in $avx.Keys) {
    $obj = $key.Split('|')[0]
    if ($obj -match '^(libucrt|libvcruntime|libcpmt|LIBCMT):' -or $obj -match '_avx2' -or $guarded[$key]) { continue }
    $key
}
Write-Host "$($avx.Count) functions use AVX instructions; all others are plain SSE2."
if ($bad) {
    Write-Host "NOT SAFE on CPUs without AVX2 - AVX instructions outside a CPU check:" -ForegroundColor Red
    $bad | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host 'OK: every AVX instruction is behind a CPU check (runtime library, *_avx2 kernels, or a compiler check).' -ForegroundColor Green
