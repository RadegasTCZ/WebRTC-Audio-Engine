// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <string>
#include <vector>

// Shared between the main settings window (SettingsApp.cpp) and the tray flyout (TrayFlyout.cpp).
namespace app
{
    // Restores and focuses the full settings window; micGuid ("{...}") preselects that microphone.
    void ShowMainWindow(const wchar_t* micGuid = nullptr);
    void ExitApp();          // removes the tray icon and quits

    // Microphones our effect pack runs on (active endpoints with "WebRTC Audio Engine" selected), plus
    // microphones that have custom settings saved even if they are not connected right now.
    struct Mic
    {
        std::wstring guid, name;   // guid = endpoint GUID "{...}", the per-device settings key
        bool connected = false;    // active endpoint with our effect pack selected
    };
    std::vector<Mic> FindMics();
    std::wstring DefaultMicGuid();  // Windows default recording device ("{...}"), or empty
}

namespace tray
{
    bool Init(HINSTANCE inst);     // tray icon + (hidden) flyout
    void Shutdown();
    void SettingsChanged();        // repaint the flyout when settings change
}
