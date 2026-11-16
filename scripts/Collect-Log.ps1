# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 RadegasTCZ
<#
.SYNOPSIS
  Copies the effect's newest log segments to a readable place and shows the end of the newest one.
  The log lives in audiodg's own temp folder (only SYSTEM, Administrators and LOCAL SERVICE can open it),
  so run this from an administrator PowerShell.
.PARAMETER Count
  How many of the newest segments to copy (default 3). Each segment is one audio session, one day or 1 MB.
.PARAMETER All
  Copy every segment (at most 10 are kept, none older than 14 days).
.PARAMETER Zip
  Also pack the copies into one .zip (for sending).
.PARAMETER Destination
  Where to put the copies. Default: <workspace>\logs, or the Desktop when run outside the workspace.
.PARAMETER Tail
  Lines of the newest segment to show (0 = none).
#>
param([int]$Count = 3, [switch]$All, [switch]$Zip, [string]$Destination, [int]$Tail = 40)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ApoConfig.ps1')
Assert-Admin

$segments = Get-ChildItem -LiteralPath $LogDir -Filter 'engine-*.log' -File -ErrorAction SilentlyContinue |
    Where-Object { -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) } | Sort-Object Name -Descending
if (-not $segments) { Write-Host "No log yet in $LogDir (the effect has not run since it was installed)."; return }
if (-not $All) { $segments = $segments | Select-Object -First $Count }

if (-not $Destination) {
    $Destination = if (Test-Path (Join-Path $WorkspaceRoot 'webrtc_driver')) { Join-Path $WorkspaceRoot 'logs' } else { [Environment]::GetFolderPath('Desktop') }
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$dir = Join-Path $Destination "WebrtcAudioEngine-logs-$stamp"
New-Item -ItemType Directory -Force $dir | Out-Null
$segments | Copy-Item -Destination $dir
Write-Host "Copied $(@($segments).Count) segment(s) to $dir"
$segments | ForEach-Object { Write-Host ('  {0}  {1,8:N0} KB' -f $_.Name, ($_.Length / 1KB)) }

if ($Zip) {
    $zipPath = "$dir.zip"
    Compress-Archive -Path (Join-Path $dir '*') -DestinationPath $zipPath -Force
    Write-Host "Packed into $zipPath"
}
if ($Tail -gt 0) {
    Write-Host "`n--- end of $(@($segments)[0].Name) ---"
    Get-Content -LiteralPath (@($segments)[0].FullName) -Tail $Tail
}
