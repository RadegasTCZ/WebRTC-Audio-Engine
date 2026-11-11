# webrtc_driver

Experimental system-wide capture APO for Windows 11 ("our own Voice Clarity"), built for a test laptop.
Milestone 1: noise suppression only (WebRTC NS), registered in the MFX and EFX slots.

**Current state:** a pass-through probe. Audio goes through unchanged, and the engine's view (slot, mode, format, period) is logged.

## Build (x64, VS 2026 + Windows SDK)
```powershell
cmake -S . -B ..\build -G "Visual Studio 18 2026" -A x64
cmake --build ..\build --config Release
..\build\Release\ProbeHarness.exe ..\build\Release\WebrtcVoiceApo.dll   # offline smoke test
```

## Install / uninstall (admin PowerShell, test machines only)
```powershell
.\scripts\Install-Probe.ps1 -DryRun     # show changes
.\scripts\Install-Probe.ps1             # install (backs up first, restarts audio)
.\scripts\Collect-ProbeLog.ps1          # copy + show C:\ProgramData\WebrtcVoiceApo\probe.log
.\scripts\Uninstall-Probe.ps1           # restore original settings
```
Build output, backups, logs and notes live next to this repo (`..\build`, `..\backups`, `..\logs`, `..\docs`), not inside it.
