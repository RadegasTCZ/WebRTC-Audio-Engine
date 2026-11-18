# Third-party code

This project is GPL-3.0-or-later (see `LICENSE`). Third-party components keep their own licences.

## WebRTC, Abseil, RNNoise weights, PFFFT (fetched, not stored in the repo)

`scripts/Fetch-Upstream.ps1` downloads the pinned sources into `<workspace>/upstream/`.
`cmake/WebrtcUpstream.cmake` builds the parts we use into the static library `webrtc_apm`:
AEC3, NS, AGC2 with the RNN VAD and limiter, AudioBuffer with its splitting filters, and their
common_audio, rtc_base, api and system_wrappers dependencies.
The file lists and revisions follow the companion AEC3-Echo-Canceller plugin (`tools/aec3-build`).

| Source | Revision | Licence |
|---|---|---|
| WebRTC, https://webrtc.googlesource.com/src | `b58dcf8fec3ad5291fb99c7e29650737ae7db953` (2026-10-02) | BSD-3-Clause + patent grant (`upstream/webrtc/LICENSE`, `PATENTS`) |
| Abseil, https://github.com/abseil/abseil-cpp | `972e4ab4a076fca3adbba69b64f2fcdbc78d0e52` | Apache-2.0 |
| Chromium `third_party/rnnoise` (RNN VAD weights) | `6eabe54fa10922ba0d3f9a8e5a01055d371ebaf7` (2026-09-24) | BSD-3-Clause |
| Chromium `third_party/pffft` | `6eabe54fa10922ba0d3f9a8e5a01055d371ebaf7` (2026-09-24) | FFTPACK-style BSD licence |

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

- Upstream: https://gitlab.xiph.org/xiph/speexdsp, version 1.2.1 (commit `7a15878`).
- Licence: BSD-3-Clause (`COPYING`; Xiph.Org Foundation, Jean-Marc Valin and others, see `AUTHORS`).
- Only the resampler is used: `resample.c` is the only compiled file, as a float build at quality 5.
- Kept headers: `arch.h`, `os_support.h`, `resample_sse.h`, `resample_neon.h`, `speex/speex_resampler.h`,
  `speex/speexdsp_types.h`, and the `fixed_*.h` / `bfin.h` headers that `arch.h` names in its fixed-point
  branches (never taken here). `COPYING` and `AUTHORS` are verbatim from the upstream root.
- Taken over from the companion Noise-suppression-VST v2.1.0 (`ThirdParty/speexdsp`).

Local changes (all other files are identical to upstream apart from line endings):
1. `resample.c` (modified): `#include "speex_config_win.h"` added at the top, and right after it
   `#define FLOATING_POINT`, `#define USE_KISS_FFT`, `#define EXPORT`, `#ifdef constant` / `#undef constant` / `#endif`.
2. `speex_config_win.h` (new, BSD-3-Clause like the rest of this folder): defines `FLOATING_POINT`,
   `USE_KISS_FFT`, `EXPORT`, undefines `constant`, and defines `USE_SSE` / `USE_SSE2` so the resampler
   uses its SSE kernels (every x64 CPU has SSE2).

## Notices shipped with the binaries

`THIRD-PARTY-NOTICES.txt` collects the full licence texts of everything above (plus WebRTC's
`common_audio/third_party/ooura` and `spl_sqrt_floor`); the MSI installs it next to `LICENSE.txt`.

## Adapted code

- `src/AgcStage.*` (AGC2 stage and -1 dBFS limiter) is adapted from AEC3-Echo-Canceller `Source/AgcStage.*` and `Source/OutputLimiter.*` (GPLv3, same author).
- `src/AecConfig.h` (echo-strength and room-size presets) is adapted from AEC3-Echo-Canceller `Source/Strength.cpp` / `AecMapping.cpp` (GPLv3, same author).
- The AGC presets (gentle / normal / strong) come from AEC3-Echo-Canceller `Source/AgcPresets.cpp`.
