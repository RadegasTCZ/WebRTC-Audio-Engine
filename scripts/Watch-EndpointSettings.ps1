# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 RadegasTCZ
<#
.SYNOPSIS
  Polls a capture endpoint's FxProperties + Properties keys (and the audio driver's class key)
  and logs every value that changes, with timestamps. Used to discover which private
  Realtek value a UI toggle writes. Read-only. A development tool: the driver-key part only
  looks for a Realtek HD Audio codec (HDAUDIO\FUNC_01&VEN_10EC); other PCs get the endpoint keys only.
#>
param(
    # Endpoint GUID, e.g. '{6a6941e8-...}' (see HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture).
    [Parameter(Mandatory)][string]$EndpointId,
    [string]$OutFile,
    [int]$IntervalMs = 150,
    [int]$Minutes = 30
)
$base = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\$EndpointId"
$keys = @("$base\FxProperties", "$base\Properties")
# Realtek HDA driver software key (stores some Realtek-wide settings).
$drv = Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceID -like 'HDAUDIO\FUNC_01&VEN_10EC*' } | Select-Object -First 1
if ($drv) {
    $drvKey = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Enum\$($drv.DeviceID)" -EA SilentlyContinue).Driver
    if ($drvKey) { $keys += "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$drvKey"; $keys += Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$drvKey" -Recurse -EA SilentlyContinue | ForEach-Object { $_.PSPath } }
}

function Snap {
    $h = @{}
    foreach ($k in $keys) {
        $item = Get-Item $k -EA SilentlyContinue
        if (-not $item) { continue }
        foreach ($n in $item.GetValueNames()) {
            $v = $item.GetValue($n, $null, 'DoNotExpandEnvironmentNames')
            $h["$($item.Name -replace '^HKEY_LOCAL_MACHINE','HKLM')|$n"] = if ($v -is [array]) { ($v | ForEach-Object { "$_" }) -join ',' } else { "$v" }
        }
    }
    $h
}

function Out([string]$s) { $line = "$(Get-Date -Format 'HH:mm:ss.fff')  $s"; Write-Host $line; if ($OutFile) { Add-Content $OutFile $line } }

$prev = Snap
Out "Watching $($keys.Count) keys, $($prev.Count) values. Toggle things now."
$end = (Get-Date).AddMinutes($Minutes)
while ((Get-Date) -lt $end) {
    Start-Sleep -Milliseconds $IntervalMs
    $cur = Snap
    foreach ($k in $cur.Keys) {
        if (-not $prev.ContainsKey($k)) { Out "NEW     $k = $($cur[$k])" }
        elseif ($prev[$k] -ne $cur[$k]) { Out "CHANGED $k : $($prev[$k])  ->  $($cur[$k])" }
    }
    foreach ($k in $prev.Keys) { if (-not $cur.ContainsKey($k)) { Out "REMOVED $k (was $($prev[$k]))" } }
    $prev = $cur
}
Out 'Watch finished.'
