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
- `app/` "WebRTC Audio Engine Settings": settings window + tray flyout with live meters
- `package/` driver packages (component + extension INF) that register the effect pack
- `scripts/` build, install and settings scripts
- `tests/` offline tests (`ProcessorTests`, `ProbeHarness`) and `CaptureTest` (live mic check)
- `third_party/` SpeexDSP resampler, WebRTC build stubs; upstream WebRTC is fetched by `scripts/Fetch-Upstream.ps1`

Build output, backups, logs and notes live next to this repo (`..\build`, `..\backups`, `..\logs`, `..\docs`), not inside it.

## Build (x64, VS 2026 + Windows SDK)
```powershell
.\scripts\Fetch-Upstream.ps1                      # once: pinned WebRTC + Abseil sources into ..\upstream
cmake -S . -B ..\build -G "Visual Studio 18 2026" -A x64
cmake --build ..\build --config Release
..\build\Release\ProcessorTests.exe               # offline tests of the processing core
..\build\Release\ProbeHarness.exe ..\build\Release\WebrtcVoiceApo.dll
```

## Install (admin PowerShell, test machines with test signing on)
```powershell
.\scripts\New-TestCert.ps1          # once: test-signing certificate (remove later with -Remove)
.\scripts\Build-Package.ps1         # stage + sign the driver packages into ..\build\package
.\scripts\Install-Package.ps1       # install the effect pack (restarts Windows Audio)
.\scripts\Install-SettingsApp.ps1   # settings app, Start-menu shortcut, tray icon at sign-in
.\scripts\Set-VoiceSettings.ps1     # show/change settings from the command line
.\scripts\Collect-ProbeLog.ps1      # copy + show the effect's log (C:\ProgramData\WebrtcVoiceApo)
```
Uninstall: `.\scripts\Uninstall-Package.ps1` and `.\scripts\Install-SettingsApp.ps1 -Uninstall`.

## Licence
GPL-3.0-or-later (see `LICENSE`). WebRTC is BSD-licensed; SpeexDSP is BSD-licensed (`third_party/speexdsp/COPYING`).
