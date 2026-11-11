# Copies the probe log from ProgramData into <workspace>\logs with a timestamp, and shows the tail.
param([int]$Tail = 40, [switch]$Clear)
. (Join-Path $PSScriptRoot 'ApoConfig.ps1')
$src = Join-Path $LogDir 'probe.log'
if (-not (Test-Path $src)) { Write-Host "No log yet at $src (the APO has not been loaded, or cannot write there)."; return }
$dstDir = Join-Path $WorkspaceRoot 'logs'
New-Item -ItemType Directory -Force $dstDir | Out-Null
$dst = Join-Path $dstDir ("probe-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
Copy-Item $src $dst
Write-Host "Saved $dst"
Get-Content $src -Tail $Tail
if ($Clear) { Clear-Content $src; Write-Host '(live log cleared)' }
