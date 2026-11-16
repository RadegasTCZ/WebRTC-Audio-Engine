#pragma once
#include <windows.h>

// Tiny append-only text log for diagnostics.
// NEVER call from APOProcess (real-time thread): it takes a lock and does file I/O.
namespace apolog
{
    // %TEMP%\WebrtcAudioEngine\engine-<start time>.log, segmented and self-cleaning (see Log.cpp). Inside
    // audiodg: C:\Windows\ServiceProfiles\LocalService\AppData\Local\Temp\WebrtcAudioEngine.
    void Write(_Printf_format_string_ const wchar_t* fmt, ...) noexcept;
    void GuidToString(REFGUID g, wchar_t (&out)[40]) noexcept;
    // Diagnostics only: writes head + data as a new file (fails if it exists) next to the log and keeps only
    // the newest few aecdump-*.wav recordings. Non-RT.
    bool WriteDiagFile(const wchar_t* name, const void* head, size_t headBytes, const void* data, size_t bytes,
                       wchar_t (&pathOut)[MAX_PATH]) noexcept;
}
