# WebRTC Audio Engine

An experimental Windows 11 microphone **effect pack** ("our own Voice Clarity"), built on upstream WebRTC audio processing.
Once installed, choose **WebRTC Audio Engine** under Settings > System > Sound > (microphone) > Audio enhancements.

Processing chain (48 kHz internally, other rates are resampled, constant latency of 1198 samples at 48 kHz):
cleanup filters (rumble 80 Hz high-pass, hiss 10 kHz low-pass) -> **AEC3** echo cancellation (render loopback reference)
-> **noise suppression** (6/12/18/21 dB) -> **AGC2** automatic gain -> -1 dBFS limiter. The mic volume slider is applied
in software where the OEM chain used to apply it.

It covers DEFAULT, COMMUNICATIONS and SPEECH capture streams; RAW streams (e.g. browsers doing their own WebRTC) are untouched.
Test-signed, for test machines only.

## Layout
- `src/` the effect (APO DLL loaded by audiodg) and the processing core
- `app/` "WebRTC Audio Engine Settings": settings window + tray flyout (microphone picker, activity dots), sharing one drawing kit (`Ui.*`)
- `tools/` build-time helpers (`MakeIcon`: generates the app icon from `app/Glyph.h`)
- `package/` driver packages: `component` (the effect), `app` (the settings app as a software component) and
  `extension` (attaches both to the PC)
- `scripts/` build, install and settings scripts
- `tests/` offline tests (`ProcessorTests`, `ApoHarness`), `CaptureTest` (live mic check) and `PathProbe` (speaker path vs echo reference)
- `third_party/` SpeexDSP resampler, WebRTC build stubs; upstream WebRTC is fetched by `scripts/Fetch-Upstream.ps1`

Build output, backups, logs and notes live next to this repo (`..\build`, `..\backups`, `..\logs`, `..\docs`), not inside it.

## Build (x64, VS 2026 + Windows SDK)
```powershell
.\scripts\Fetch-Upstream.ps1                      # once: pinned WebRTC + Abseil sources into ..\upstream
cmake -S . -B ..\build -G "Visual Studio 18 2026" -A x64
cmake --build ..\build --config Release
..\build\Release\ProcessorTests.exe               # offline tests of the processing core
..\build\Release\ApoHarness.exe ..\build\Release\WebrtcVoiceApo.dll
.\scripts\Check-CpuSafety.ps1                     # every AVX instruction is behind a CPU check (runs on any x64 CPU)
```

## Install (admin PowerShell)
```powershell
.\scripts\New-TestCert.ps1          # once: test-signing certificate (remove later with -Remove)
.\scripts\Build-Package.ps1         # stage + sign the driver packages into ..\build\package
.\scripts\Install-Package.ps1       # install the effect pack + settings app (restarts Windows Audio)
.\scripts\Install-SettingsApp.ps1   # optional: update just the settings app from the build folder
.\scripts\Set-VoiceSettings.ps1     # show/change settings from the command line
.\scripts\Collect-Log.ps1           # copy + show the effect's log (C:\ProgramData\WebrtcVoiceApo\engine.log)
```
Installing the driver installs the settings app too: Windows runs `WebrtcAudioEngineSettings.exe /install` from the
package, which adds it to Program Files, the Start menu, sign-in (tray icon) and Apps & features
(log: `%ProgramData%\WebrtcAudioEngine\setup.log`). Removing the app in Apps & features keeps the effect; the app
comes back with the next driver version.

Uninstall everything: `.\scripts\Uninstall-Package.ps1` (settings are kept).

Test signing mode is not needed: the packages are user-mode only, so Windows only has to trust the signing
certificate (New-TestCert.ps1 puts it in LocalMachine Root + TrustedPublisher).

## Installer (MSI)
```powershell
dotnet tool install --global wix --version 7.0.1   # once (.NET SDK needed); then: wix eula accept wix7
.\scripts\Build-Package.ps1
.\scripts\Build-Msi.ps1                            # -> ..\build\msi\WebrtcAudioEngine-<version>.msi
```
The MSI (installer/Product.wxs) copies the signed packages to Program Files\WebrtcAudioEngine\Driver. Its custom
action (installer/SetupActions.cpp) trusts the bundled certificate (TrustedPublisher, plus Root if self-made),
installs the packages and restarts Windows Audio. It is the only "Installed apps" entry: removing it removes the
drivers, the settings app and any certificate trust it added (settings are kept). An upgrade removes the old
version first. Requires Windows 11.

## Licence
GPL-3.0-or-later (see `LICENSE`). WebRTC is BSD-licensed; SpeexDSP is BSD-licensed (`third_party/speexdsp/COPYING`).
