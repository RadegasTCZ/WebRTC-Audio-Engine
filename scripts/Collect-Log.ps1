# Copies the effect's log from ProgramData into <workspace>\logs with a timestamp, and shows the tail.
param([int]$Tail = 40)
. (Join-Path $PSScriptRoot 'ApoConfig.ps1')
$src = Join-Path $LogDir 'engine.log'
if (-not (Test-Path $src)) { Write-Host "No log yet at $src (the effect has not run yet, or cannot write there)."; return }
$dstDir = Join-Path $WorkspaceRoot 'logs'
New-Item -ItemType Directory -Force $dstDir | Out-Null
$dst = Join-Path $dstDir ("engine-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
Copy-Item $src $dst
Write-Host "Saved $dst"
Get-Content $src -Tail $Tail
