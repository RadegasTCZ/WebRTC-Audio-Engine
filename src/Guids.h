#pragma once
#include <guiddef.h>

// Our two APO classes. Keep these in sync with scripts\ApoConfig.ps1.
// {50E690BC-E5D5-4D62-84DF-D50D2FD20E16}  probe in the per-mode slot (MFX)
inline constexpr CLSID CLSID_WebrtcVoiceApoMFX =
    {0x50e690bc, 0xe5d5, 0x4d62, {0x84, 0xdf, 0xd5, 0x0d, 0x2f, 0xd2, 0x0e, 0x16}};

// {F017616C-C6AE-4B1D-942A-D0D1DD5FEB74}  probe in the per-endpoint slot (EFX)
inline constexpr CLSID CLSID_WebrtcVoiceApoEFX =
    {0xf017616c, 0xc6ae, 0x4b1d, {0x94, 0x2a, 0xd0, 0xd1, 0xdd, 0x5f, 0xeb, 0x74}};
