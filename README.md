# WebRTC Audio Engine

Author and maintainer: Radek Boruta ([RadegasTCZ](https://github.com/RadegasTCZ))

A Windows 11 microphone **effect pack** (a Voice Clarity-style effect), built on upstream WebRTC audio processing.
Once installed, choose **WebRTC Audio Engine** under Settings > System > Sound > (microphone) > Audio enhancements.

Processing chain (48 kHz internally, other rates are resampled; the effect always adds exactly 50 ms of latency):
cleanup filters (rumble high-pass 60/80/120 Hz, hiss low-pass 10/9/8 kHz, gentle/normal/strong) -> **AEC3** echo cancellation (with the speakers'
loopback as reference, automatic echo timing and clock-rate tracking) -> **noise suppression** (6/12/18/21 dB)
-> **AGC2** automatic gain -> -1 dBFS limiter. The mic volume slider is applied in software where the OEM chain
used to apply it.

It covers DEFAULT, COMMUNICATIONS and SPEECH capture streams; RAW streams (e.g. browsers doing their own WebRTC)
are untouched. Because the echo reference is everything the PC plays, it also removes game or music audio that
leaks into a headset mic, which per-app echo cancellation (Discord, Teams) does not.

**Status:** works on the developer's machines and a few test PCs (Realtek HD Audio, SoundWire, USB mics).
Official releases, published on this repository's Releases page, are signed with a Certum open-source
code-signing certificate ("Open Source Developer Radek Boruta", issued by Certum Code Signing 2021 CA) and
timestamped. Any other copy, including one you build yourself, is used at your own risk.

**What the installer changes on your PC's trust settings:** the driver packages are signed by the author, not by
Microsoft. Windows installs such packages silently only if their publisher is trusted, so the MSI adds the
certificate it ships with (public part only) to the computer's **Trusted Publishers** store. It is not added to
the trusted root authorities: Certum's root is already trusted by Windows. While it is there, Windows installs
any driver package signed with this certificate without asking. Uninstalling removes the trust again (if the
installer was the one that added it). To check: `certlm.msc` > Trusted Publishers > Certificates.

The effect-pack mechanism uses an undocumented Windows schema, so a Windows update could break it.

WebRTC Audio Engine is an independent project. It is not affiliated with or endorsed by Microsoft, Google, the WebRTC
project or any other upstream author.

## Privacy
Everything runs locally; there is no network code. The effect writes a diagnostic log on the PC (see below);
it records settings, timings and levels, never audio. An administrator can switch on a short diagnostic audio
recording (`DiagDumpSeconds` under `HKLM\SOFTWARE\WebrtcAudioEngine\Diagnostics`); it is off by default.

## Layout
- `src/` the effect (APO DLL loaded by audiodg) and the processing core
- `app/` "WebRTC Audio Engine Settings": settings window + tray flyout (microphone picker, activity dots), sharing one drawing kit (`Ui.*`)
- `installer/` the MSI (`Product.wxs`) and its custom action (`SetupActions.cpp`)
- `tools/` build-time helpers (`MakeIcon`: generates the app icon from `app/Glyph.h`)
- `package/` driver packages: `component` (the effect), `app` (the settings app as a software component) and
  `extension` (attaches both to the PC)
- `scripts/` build, install and settings scripts
- `tests/` offline tests (`ProcessorTests`, `ApoHarness`), live checks (`CaptureTest`: mic; `PathProbe`: speaker path
  vs echo reference; `ClockProbe`: real device clock rates) and `AecDumpXcorr` (echo timing in a diagnostic recording)
- `third_party/` SpeexDSP resampler and WebRTC build stubs; see `third_party/UPSTREAM.md`

Build output, upstream sources and logs live **next to** the clone, not inside it: `..\build`, `..\upstream`,
`..\logs`. Clone into a folder of its own (e.g. `C:\src\webrtc-audio-engine\webrtc_driver`).

## Prerequisites
Built and tested with:
- Windows 11 x64
- Visual Studio 2026 (MSVC 14.52) with the "Desktop development with C++" workload
- Windows SDK and **Windows Driver Kit** 10.0.28000 (the WDK provides `Inf2Cat.exe`)
- CMake 4.4 (any version with the "Visual Studio 18 2026" generator)
- PowerShell 7.7 (Windows PowerShell 5.1 works for the scripts too) and the built-in `tar.exe`
- For the MSI: .NET SDK 10 and WiX 7.0.1
- Admin rights for `New-TestCert.ps1` and anything that installs

## Build (x64)
```powershell
.\scripts\Fetch-Upstream.ps1                      # once: pinned WebRTC + Abseil + Chromium sources into ..\upstream (content-checked)
cmake -S . -B ..\build -G "Visual Studio 18 2026" -A x64
cmake --build ..\build --config Release
..\build\Release\ProcessorTests.exe               # offline tests of the processing core
..\build\Release\ApoHarness.exe ..\build\Release\WebrtcVoiceApo.dll
.\scripts\Check-CpuSafety.ps1                     # every AVX instruction is behind a CPU check (runs on any x64 CPU)
```

## Install for development (admin PowerShell)
```powershell
.\scripts\New-TestCert.ps1          # once: test-signing certificate (remove later with -Remove)
.\scripts\Build-Package.ps1         # stage + sign the driver packages into ..\build\package
.\scripts\Install-Package.ps1       # install the effect pack + settings app (restarts Windows Audio)
.\scripts\Install-SettingsApp.ps1   # optional: update just the settings app from the build folder
.\scripts\Set-VoiceSettings.ps1     # show/change settings from the command line
.\scripts\Collect-Log.ps1           # copy the newest log segments to ..\logs and show the end (-All, -Zip)
```
The effect logs to audiodg's own temp folder, `C:\Windows\ServiceProfiles\LocalService\AppData\Local\Temp\WebrtcAudioEngine`
(admins only), as `engine-<start time>.log` segments: a new one per audio session, per day and per 1 MB. It keeps
the newest 10, none older than 14 days; uninstalling removes them.

Installing the driver installs the settings app too: Windows runs `WebrtcAudioEngineSettings.exe /install` from the
package, which adds it to Program Files, the Start menu, sign-in (tray icon) and, when installed without the
MSI, its own Apps & features entry (log: `%ProgramData%\WebrtcAudioEngine\setup.log`).

Uninstall everything: `.\scripts\Uninstall-Package.ps1` (settings are kept).

Test signing mode is not needed: the packages are user-mode only, so Windows only has to trust the signing
certificate (New-TestCert.ps1 puts it in LocalMachine Root + TrustedPublisher). Tested with Secure Boot on.

## Installer (MSI)
```powershell
dotnet tool install --global wix --version 7.0.1   # once (.NET SDK needed); then: wix eula accept wix7
.\scripts\Build-Package.ps1
.\scripts\Build-Msi.ps1                            # -> ..\build\msi\WebrtcAudioEngine-<version>.msi
```
The MSI copies the signed packages to Program Files\WebrtcAudioEngine\Driver, together with `LICENSE.txt` and
`THIRD-PARTY-NOTICES.txt`. Its custom action trusts the bundled certificate (TrustedPublisher, plus Root if
self-made, as in builds made with these scripts), installs the packages and restarts Windows Audio (a repair of
the same version leaves Windows Audio running). It is the only "Installed apps" entry: removing it removes the drivers, the settings app and any
certificate trust it added (settings are kept). An upgrade removes the old version first. Requires Windows 11 x64.

## Licence
Copyright (C) 2026 Radek Boruta (RadegasTCZ). GPL-3.0-or-later (see `LICENSE`). The binaries also contain WebRTC (BSD-3-Clause + patent grant), Abseil
(Apache-2.0), RNNoise (BSD-3-Clause), PFFFT (BSD-style) and the SpeexDSP resampler (BSD-3-Clause); their full
licence texts are in `THIRD-PARTY-NOTICES.txt`, and `third_party/UPSTREAM.md` lists the exact revisions and
local changes.
