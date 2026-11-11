#pragma once
#include <guiddef.h>

// Our two APO classes. Keep these in sync with scripts\ApoConfig.ps1.
// {50E690BC-E5D5-4D62-84DF-D50D2FD20E16}  probe in the per-mode slot (MFX)
inline constexpr CLSID CLSID_WebrtcVoiceApoMFX =
    {0x50e690bc, 0xe5d5, 0x4d62, {0x84, 0xdf, 0xd5, 0x0d, 0x2f, 0xd2, 0x0e, 0x16}};

// {F017616C-C6AE-4B1D-942A-D0D1DD5FEB74}  probe in the per-endpoint slot (EFX)
inline constexpr CLSID CLSID_WebrtcVoiceApoEFX =
    {0xf017616c, 0xc6ae, 0x4b1d, {0x94, 0x2a, 0xd0, 0xd1, 0xdd, 0x5f, 0xeb, 0x74}};

// {2BB71AC0-6882-4965-BC38-CA716D62C0E2}  "WebRTC Audio Engine" effect pack APO (mode effect, like Voice Clarity).
// Keep in sync with package\component\WebrtcAudioEngine.inf.
inline constexpr CLSID CLSID_WebrtcAudioEnginePack =
    {0x2bb71ac0, 0x6882, 0x4965, {0xbc, 0x38, 0xca, 0x71, 0x6d, 0x62, 0xc0, 0xe2}};
