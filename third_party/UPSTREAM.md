# Third-party code

This project is GPL-3.0-or-later (see `LICENSE`). Third-party components keep their own licences.

## WebRTC, Abseil, RNNoise weights, PFFFT (fetched, not stored in the repo)

`scripts/Fetch-Upstream.ps1` downloads the pinned sources into `<workspace>/upstream/`.
`cmake/WebrtcUpstream.cmake` builds the parts we use into the static library `webrtc_apm`:
AEC3, NS, AGC2 with the RNN VAD and limiter, AudioBuffer with its splitting filters, and their
common_audio, rtc_base, api and system_wrappers dependencies.
The file lists and revisions follow the owner's AEC3-Echo-Canceller plugin (`tools/aec3-build`).

| Source | Revision | Licence |
|---|---|---|
| WebRTC, https://webrtc.googlesource.com/src | `b58dcf8fec3ad5291fb99c7e29650737ae7db953` (2026-10-02) | BSD-3-Clause + patent grant (`upstream/webrtc/LICENSE`, `PATENTS`) |
| Abseil, https://github.com/abseil/abseil-cpp | `972e4ab4a076fca3adbba69b64f2fcdbc78d0e52` | Apache-2.0 |
| Chromium `third_party/rnnoise` (RNN VAD weights) | `6eabe54fa1` | BSD-3-Clause |
| Chromium `third_party/pffft` | `6eabe54fa1` | FFTPACK-style BSD licence |

No upstream file is modified. Two notes on the build:
- The MSVC build links `winmm` (`timeGetTime`).
- `matched_filter_avx2.cc` is replaced by a stub, because it uses a GCC vector extension.

## `third_party/webrtc_stubs/`

Four small files copied from the AEC3-Echo-Canceller plugin (`tools/aec3-build/stubs`):
- a task-queue factory that is never run;
- `RtcEventLogNull::StartLogging`;
- the AVX2 matched filter routed to SSE2;
- an empty field-trial list.

## `third_party/speexdsp/`: SpeexDSP resampler

- Copied unchanged from the owner's Noise-suppression-VST v2.1.0 (`ThirdParty/speexdsp`), which records upstream 1.2.1 and its two local Windows-config patches in its own `ThirdParty/UPSTREAM.md`.
- BSD-3-Clause (`COPYING`).
- Only `resample.c` and its headers are used, as a float build with quality 5.

## Adapted code

- `src/AgcStage.*` (AGC2 stage and -1 dBFS limiter) is adapted from AEC3-Echo-Canceller `Source/AgcStage.*` and `Source/OutputLimiter.*` (GPLv3, same author).
- The AGC presets (gentle / normal / strong) come from AEC3-Echo-Canceller `Source/AgcPresets.cpp`.
