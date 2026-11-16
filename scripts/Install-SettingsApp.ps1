# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 RadegasTCZ
<#
.SYNOPSIS
  Installs just "WebRTC Audio Engine Settings" from the build folder (admin), for quick app updates while
  developing; Install-Package.ps1 installs it as part of the driver. Runs the app's own installer
  ("/install"): Program Files, Start-menu shortcut, tray icon at every sign-in, Apps & features entry,
  settings key writable by Users. Undo with -Uninstall (settings are kept).
#>
param([string]$ExePath, [switch]$Uninstall)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ApoConfig.ps1')
Assert-Admin

if ($Uninstall) { Uninstall-SettingsApp; return }

if (-not $ExePath) { $ExePath = $DefaultAppExe }
if (-not (Test-Path $ExePath)) { throw "Not found: $ExePath (build first)" }
$code = Invoke-AppSetup $ExePath '/install dev'
if ($code) { throw "Install failed (code $code); see $SetupLog" }
Write-Host "Installed $InstalledAppExe, the Start-menu shortcut and the tray icon (also starts at sign-in)."
