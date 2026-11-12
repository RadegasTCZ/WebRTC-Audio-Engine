<#
.SYNOPSIS
  Builds the installer ..\build\msi\WebrtcAudioEngine-<version>.msi from the driver packages that
  .\Build-Package.ps1 staged in ..\build\package, and signs it.
  Needs the WiX tool (dotnet tool install --global wix --version 7.0.1) and a CMake Release build
  (SetupActions.dll).
  Install: double-click the .msi, or msiexec /i <msi> /l*v install.log. Remove it from Installed apps.
#>
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageConfig.ps1')

$wix = Join-Path $env:USERPROFILE '.dotnet\tools\wix.exe'
if (-not (Test-Path $wix)) { throw 'WiX not found: dotnet tool install --global wix --version 7.0.1' }
if (-not $env:PATH.Contains('dotnet')) { $env:PATH = "$env:ProgramFiles\dotnet;$env:PATH" }

$cert = Get-TestCert
if (-not $cert) { throw 'No signing certificate. Run .\New-TestCert.ps1 once (admin).' }
$signtool = Find-KitTool 'signtool.exe'

$componentInf = Join-Path $PackageOut "component\$ComponentInf"
if (-not (Test-Path $componentInf)) { throw "Missing $componentInf (run .\Build-Package.ps1)" }
if ((Get-Content $componentInf -Raw) -notmatch 'DriverVer\s*=\s*[^,]+,\s*(\d+\.\d+\.\d+\.\d+)') { throw "No DriverVer in $componentInf" }
$version = $Matches[1]

$caDll = Join-Path $WorkspaceRoot 'build\Release\SetupActions.dll'
$icon = Join-Path $WorkspaceRoot 'build\generated\WebrtcAudioEngine.ico'
foreach ($f in $caDll, $icon) { if (-not (Test-Path $f)) { throw "Missing $f (cmake --build ..\build --config Release)" } }

$out = Join-Path $WorkspaceRoot 'build\msi'
$stage = Join-Path $out 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item $caDll $stage
$stagedCa = Join-Path $stage 'SetupActions.dll'
& $signtool sign /q /sm /s My /sha1 $cert.Thumbprint /fd sha256 $stagedCa
if ($LASTEXITCODE) { throw 'signtool failed on SetupActions.dll' }
$cer = Join-Path $stage 'WebrtcAudioEngine.cer'
Export-Certificate -Cert $cert -FilePath $cer | Out-Null

$msi = Join-Path $out "WebrtcAudioEngine-$version.msi"
Write-Host "Building $msi ..."
& $wix build (Join-Path $RepoRoot 'installer\Product.wxs') -arch x64 -nologo `
    -d "ProductVersion=$version" -d "PackageDir=$PackageOut" -d "CertFile=$cer" -d "CaDll=$stagedCa" -d "IconFile=$icon" `
    -o $msi
if ($LASTEXITCODE) { throw 'wix build failed' }
& $signtool sign /q /sm /s My /sha1 $cert.Thumbprint /fd sha256 /d 'WebRTC Audio Engine' $msi
if ($LASTEXITCODE) { throw 'signtool failed on the MSI' }
Remove-Item (Join-Path $out '*.wixpdb') -ErrorAction SilentlyContinue
Write-Host "Ready: $msi ($([math]::Round((Get-Item $msi).Length / 1MB, 1)) MB)"
