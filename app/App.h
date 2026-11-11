// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>

// Shared between the main settings window (SettingsApp.cpp) and the tray flyout (TrayFlyout.cpp).
namespace app
{
    void ShowMainWindow();   // restores and focuses the full settings window
    void ExitApp();          // removes the tray icon and quits
}

namespace tray
{
    bool Init(HINSTANCE inst);     // tray icon + (hidden) flyout
    void Shutdown();
    void SettingsChanged();        // repaint the flyout when settings change
}
