# Third-party code

## WebRTC noise suppressor: `third_party/webrtc/`

- Taken from the owner's plugin repo https://github.com/RadegasTCZ/Noise-suppression-VST,
  v2.1.0, commit `b4d0309c9add167819c0bc3b2b4ccef0f586150f`, folder `ThirdParty/webrtc/`. Copied unchanged.
- That copy comes from upstream https://webrtc.googlesource.com/src, revision
  `b58dcf8fec3ad5291fb99c7e29650737ae7db953` (2026-10-02).
- License: BSD-3-Clause (`webrtc/LICENSE`) plus the WebRTC patent grant (`webrtc/PATENTS`). The Ooura FFT has its own
  permissive license (`webrtc/common_audio/third_party/ooura/LICENSE`).
- Contents:
  - `modules/audio_processing/ns/*`
  - `modules/audio_processing/three_band_filter_bank.*`
  - `common_audio/third_party/ooura/fft_size_256/fft4g.*`
  - shims for `rtc_base/checks.h` and `modules/audio_processing/audio_buffer.h`, under `shim/`
- Local patch (inherited from the plugin): `NoiseSuppressor::Reset()` restores the constructor state without allocating.
  The shims and this patch are documented in the plugin's `ThirdParty/UPSTREAM.md`.
- Behaviour:
  - The three-band filter bank plus the NS overlap delay the signal by 46 + 3*96 = 334 samples at 48 kHz.
  - NS works in the int16 range, so we scale by 32768 on the way in and 1/32768 on the way out.
