# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 RadegasTCZ
<#
.SYNOPSIS
  One-time: creates a self-made code-signing certificate for test-signing our driver packages and
  trusts it on THIS machine (LocalMachine Root + TrustedPublisher). Test machines only.
  Remove with -Remove.
#>
param([switch]$Remove)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageConfig.ps1')
Assert-Admin

if ($Remove) {
    foreach ($store in 'My', 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Subject -eq $CertSubject | ForEach-Object {
            Remove-Item $_.PSPath; Write-Host "Removed from LocalMachine\$store : $($_.Thumbprint)"
        }
    }
    return
}

$cert = Get-TestCert
if ($cert) { Write-Host "Test certificate already exists: $($cert.Thumbprint)"; return }

$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject -CertStoreLocation Cert:\LocalMachine\My `
        -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(3)
New-Item -ItemType Directory -Force $PackageOut | Out-Null
$cer = Join-Path $PackageOut 'WebrtcAudioEngine-TestCert.cer'
Export-Certificate -Cert $cert -FilePath $cer | Out-Null
foreach ($store in 'Root', 'TrustedPublisher') {
    Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
}
Write-Host "Created and trusted test certificate $($cert.Thumbprint) ($CertSubject)"
