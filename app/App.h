// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <string>
#include <vector>

// Shared between the main settings window (SettingsApp.cpp) and the tray flyout (TrayFlyout.cpp).
namespace app
{
    inline constexpr wchar_t kExeName[] = L"WebrtcAudioEngineSettings.exe";
    inline constexpr wchar_t kWindowClass[] = L"WebrtcAudioEngineSettings";  // main window (finds the running copy)
    // HKLM key only admins may write (Users read and watch it). AppGeneration in it: the installer bumps it,
    // every running copy exits when it changes (Setup.cpp).
    inline constexpr wchar_t kAppKey[] = L"SOFTWARE\\WebrtcAudioEngine";
    inline constexpr wchar_t kGenerationValue[] = L"AppGeneration";
    DWORD AppGeneration();

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

namespace setup
{
    int Install(const wchar_t* cmdLine);    // "/install [version]": Program Files, shortcut, sign-in start, Apps & features
    int Uninstall();                        // "/uninstall": removes all that (settings are kept)
}
