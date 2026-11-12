/* Copyright (c) 2026 RadegasTCZ. Local file of Noise Suppression (not upstream); BSD-3-Clause, see COPYING. */
/* Windows/MSVC build configuration for the vendored SpeexDSP sources (float build, Kiss FFT). */
#ifndef SPEEX_CONFIG_WIN_H
#define SPEEX_CONFIG_WIN_H

#define FLOATING_POINT
#define USE_KISS_FFT
/* SSE/SSE2 kernels of the resampler: every x64 CPU has both, so no runtime check is needed. */
#define USE_SSE
#define USE_SSE2
#define EXPORT

#ifdef constant
#undef constant
#endif

#endif // SPEEX_CONFIG_WIN_H