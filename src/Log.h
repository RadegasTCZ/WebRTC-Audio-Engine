#pragma once
#include <windows.h>

// Tiny append-only text log for diagnostics.
// NEVER call from APOProcess (real-time thread): it takes a lock and does file I/O.
namespace apolog
{
    // C:\ProgramData\WebrtcVoiceApo\engine.log (folder is created + ACL'd by the install script).
    void Write(_Printf_format_string_ const wchar_t* fmt, ...) noexcept;
    void GuidToString(REFGUID g, wchar_t (&out)[40]) noexcept;
}
