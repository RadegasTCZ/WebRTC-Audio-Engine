// SPDX-License-Identifier: GPL-3.0-or-later
// Tray icon + a home-brew flyout: pick a microphone, then quick switches for echo cancellation, noise
// suppression, automatic gain and cleanup, each with an activity dot and a one-line status from the
// audio effect's live telemetry. Drawn with the shared kit (Ui.h); follows the light/dark theme.
//
// Dots: lit = working, amber = holding/waiting, filled grey = switched off, hollow ring = this
// microphone is not recording, red = the limiter is catching peaks (on the automatic gain row).
#include "App.h"
#include "Ui.h"
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cwchar>
#include <cmath>
#include <string>
#include "../src/Settings.h"
#include "../src/Telemetry.h"
#include "Glyph.h"

namespace
{
    constexpr UINT WM_TRAY = WM_APP + 1;
    constexpr UINT kTrayId = 1;
    constexpr UINT_PTR kMeterTimer = 1;
    enum MenuCmd : UINT { CMD_OPEN = 1, CMD_EXIT };
    constexpr wchar_t kPrefsKey[] = L"Software\\WebrtcAudioEngine";  // per user: the flyout's microphone

    HINSTANCE g_inst = nullptr;
    HWND g_flyout = nullptr;
    HICON g_icon = nullptr;
    NOTIFYICONDATAW g_nid{};
    UINT g_taskbarCreated = 0;
    ULONGLONG g_hiddenAt = 0;  // a click on the tray icon right after the flyout lost focus should not reopen it

    // ---------------------------------------------------------------- tray icon (drawn at runtime)

    HICON MakeIcon(int size)
    {
        BITMAPV5HEADER bi{};
        bi.bV5Size = sizeof(bi);
        bi.bV5Width = size;
        bi.bV5Height = -size;
        bi.bV5Planes = 1;
        bi.bV5BitCount = 32;
        bi.bV5Compression = BI_BITFIELDS;
        bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
        void* bits = nullptr;
        HDC dc = GetDC(nullptr);
        HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, dc);
        if (!color || !bits)
        {
            if (color) DeleteObject(color);
            return nullptr;  // no icon this time (out of GDI resources); the tray keeps working
        }
        // Plain bars in the taskbar's text color (no tile: tray icons are monochrome on Windows 11).
        glyph::Render(size, static_cast<uint32_t*>(bits), ui::IsDark() ? 0xFFFFFF : 0x202020);
        HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
        ICONINFO ii{TRUE, 0, 0, mask, color};
        HICON icon = CreateIconIndirect(&ii);
        DeleteObject(mask);
        DeleteObject(color);
        return icon;
    }

    void AddTrayIcon()
    {
        if (g_icon) DestroyIcon(g_icon);
        g_icon = MakeIcon(GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem()));
        g_nid = {sizeof(g_nid)};
        g_nid.hWnd = g_flyout;
        g_nid.uID = kTrayId;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        g_nid.uCallbackMessage = WM_TRAY;
        g_nid.hIcon = g_icon;
        wcscpy_s(g_nid.szTip, L"WebRTC Audio Engine");
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    }

    // ---------------------------------------------------------------- which microphone

    std::vector<app::Mic> g_mics;
    std::wstring g_micGuid;  // the flyout's microphone; empty = none uses the effect pack (defaults shown)

    std::wstring LoadPreferredMic()
    {
        wchar_t v[128] = L"";
        DWORD size = sizeof(v);
        if (RegGetValueW(HKEY_CURRENT_USER, kPrefsKey, L"FlyoutMic", RRF_RT_REG_SZ, nullptr, v, &size) != ERROR_SUCCESS) return L"";
        return v;
    }
    void SavePreferredMic(const std::wstring& guid)
    {
        HKEY k = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kPrefsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS)
        {
            RegSetValueExW(k, L"FlyoutMic", 0, REG_SZ, reinterpret_cast<const BYTE*>(guid.c_str()),
                           static_cast<DWORD>((guid.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(k);
        }
    }

    int MicIndex(const std::wstring& guid)
    {
        for (size_t i = 0; i < g_mics.size(); ++i)
            if (!guid.empty() && _wcsicmp(g_mics[i].guid.c_str(), guid.c_str()) == 0) return static_cast<int>(i);
        return -1;
    }

    // Refreshes the list and keeps the choice: the current one, else the remembered one, else the Windows
    // default microphone, else one that is recording, else the first.
    void RefreshMics()
    {
        g_mics = app::FindMics();
        if (MicIndex(g_micGuid) >= 0) return;
        g_micGuid = LoadPreferredMic();
        if (MicIndex(g_micGuid) >= 0) return;
        g_micGuid = app::DefaultMicGuid();
        if (MicIndex(g_micGuid) >= 0) return;
        telemetry::Device live[telemetry::kMaxSlots];
        const int n = telemetry::ReadAll(live);
        g_micGuid = n > 0 && MicIndex(live[0].device) >= 0 ? live[0].device : !g_mics.empty() ? g_mics[0].guid : L"";
    }

    const wchar_t* MicName()
    {
        const int i = MicIndex(g_micGuid);
        return i >= 0 ? g_mics[static_cast<size_t>(i)].name.c_str() : L"No microphone uses the engine";
    }
    // Where the switches write: this microphone's own settings if it has custom ones, else the defaults.
    const wchar_t* Scope() { return !g_micGuid.empty() && Settings::IsDeviceCustom(g_micGuid.c_str()) ? g_micGuid.c_str() : nullptr; }

    // This microphone's effective settings (its own if custom, else the defaults). Read from the registry
    // only when something changed (switch flipped, microphone picked, settings changed elsewhere).
    Settings g_settings{};
    void ReloadSettings() { g_settings = Settings::ReadFromRegistry(g_micGuid.empty() ? nullptr : g_micGuid.c_str()); }

    // ---------------------------------------------------------------- live state (100 ms timer)

    bool g_recording = false;
    telemetry::Values g_live{};
    bool g_speech = false;          // with ~300 ms hold, so the dots don't flicker on every syllable
    ULONGLONG g_speechAt = 0, g_alertUntil = 0;

    float Sane(float v, float lo, float hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : lo; }

    void ReadLive()
    {
        telemetry::Device live[telemetry::kMaxSlots];
        const int n = telemetry::ReadAll(live);
        g_recording = false;
        for (int k = 0; k < n; ++k)
            if (!g_micGuid.empty() && _wcsicmp(live[k].device, g_micGuid.c_str()) == 0)
            {
                g_recording = true;
                g_live = live[k].values;
            }
        const ULONGLONG now = GetTickCount64();
        if (g_recording && Sane(g_live.speech, 0.0f, 1.0f) >= 0.5f) { g_speech = true; g_speechAt = now; }
        else if (now - g_speechAt > 300) g_speech = false;
        if (g_recording && Sane(g_live.limiterDb, 0.0f, 99.0f) > 1.0f) g_alertUntil = now + 300;
    }

    // ---------------------------------------------------------------- layout + drawing

    enum Row { ROW_AEC, ROW_NS, ROW_AGC, ROW_CLEANUP, ROW_COUNT };
    const wchar_t* kRowNames[ROW_COUNT] = {L"Echo cancellation", L"Noise suppression", L"Automatic gain", L"Cleanup filters"};
    // Keyboard focus: the microphone field, the four switches, the "More settings" link.
    enum Focus { F_MIC = -1, F_LINK = ROW_COUNT };
    int g_focus = F_MIC;
    bool g_cues = false;
    int g_hot = -2;  // control under the mouse (-2 none)

    UINT Dpi() { return GetDpiForWindow(g_flyout); }
    int S(int v) { return ui::Px(v, Dpi()); }
    constexpr int kWidth = 320, kPad = 14, kTitleH = 40, kMicTop = 40, kMicH = 36, kScopeTop = 80, kCardTop = 106, kRowH = 50, kLinkH = 40;
    constexpr int kHeight = kCardTop + 8 + ROW_COUNT * kRowH + 8 + kLinkH;

    RECT MicRect() { return {S(kPad), S(kMicTop), S(kWidth - kPad), S(kMicTop + kMicH)}; }
    RECT RowRect(int row) { return {S(kPad), S(kCardTop + 8 + row * kRowH), S(kWidth - kPad), S(kCardTop + 8 + (row + 1) * kRowH)}; }
    RECT LinkRect() { return {S(kPad), S(kHeight - kLinkH + 4), S(kPad + 140), S(kHeight - 6)}; }

    bool RowOn(const Settings& s, int row)
    {
        switch (row)
        {
        case ROW_AEC: return s.aecEnabled;
        case ROW_NS: return s.nsEnabled;
        case ROW_AGC: return s.agcEnabled;
        default: return s.hpfEnabled || s.lpfEnabled;
        }
    }

    // The dot and the status line under a row's name.
    ui::Dot RowState(const Settings& s, int row, std::wstring& detail)
    {
        if (!RowOn(s, row)) { detail = L"Off"; return ui::Dot::Off; }
        if (!g_recording)
        {
            const int i = MicIndex(g_micGuid);
            detail = i >= 0 && !g_mics[static_cast<size_t>(i)].connected ? L"Microphone not connected" : L"Not recording";
            return ui::Dot::Idle;
        }
        wchar_t buf[64];
        switch (row)
        {
        case ROW_AEC:
            if (!g_live.aecRunning) { detail = L"No speaker reference for this recording"; return ui::Dot::Hold; }
            if (!(g_live.referenceDbfs >= -70.0f)) { detail = L"Waiting: nothing playing"; return ui::Dot::Hold; }
            swprintf_s(buf, L"Removing echo \x00B7 %.0f dB", Sane(g_live.echoRemovedDb, 0.0f, 99.0f));
            detail = buf;
            return ui::Dot::Lit;
        case ROW_NS:
            if (g_speech) { detail = L"Holding the noise profile (speech)"; return ui::Dot::Hold; }
            detail = L"Learning the background noise";
            return ui::Dot::Lit;
        case ROW_AGC:
        {
            const float gain = Sane(g_live.agcGainDb, -99.0f, 99.0f);
            if (GetTickCount64() < g_alertUntil)
            {
                swprintf_s(buf, L"Limiter catching peaks \x00B7 %+.0f dB", gain);
                detail = buf;
                return ui::Dot::Alert;
            }
            swprintf_s(buf, g_speech ? L"Adjusting \x00B7 %+.0f dB" : L"Holding \x00B7 %+.0f dB", gain);
            detail = buf;
            return g_speech ? ui::Dot::Lit : ui::Dot::Hold;
        }
        default:
            detail = s.hpfEnabled && s.lpfEnabled ? L"Rumble and hiss" : s.hpfEnabled ? L"Rumble (below 80 Hz)" : L"Hiss (above 10 kHz)";
            return ui::Dot::Lit;
        }
    }

    // Fonts, made once per DPI.
    struct
    {
        UINT dpi = 0;
        HFONT title = nullptr, body = nullptr, note = nullptr, icons = nullptr;
    } g_fonts;
    void Fonts(UINT dpi)
    {
        if (g_fonts.dpi == dpi) return;
        for (HFONT f : {g_fonts.title, g_fonts.body, g_fonts.note, g_fonts.icons})
            if (f) DeleteObject(f);
        g_fonts = {dpi, ui::Font(11, FW_SEMIBOLD, dpi), ui::Font(10, FW_NORMAL, dpi), ui::Font(8, FW_NORMAL, dpi), ui::IconFont(10, dpi)};
    }

    // What the dots and status lines show right now; the timer repaints only when this changes.
    std::wstring g_shown;
    std::wstring LiveView()
    {
        std::wstring v;
        for (int row = 0; row < ROW_COUNT; ++row)
        {
            std::wstring detail;
            v += static_cast<wchar_t>(L'0' + static_cast<int>(RowState(g_settings, row, detail)));
            v += detail;
            v += L'|';
        }
        return v;
    }

    void Paint(HWND hwnd)
    {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bmp = CreateCompatibleBitmap(wdc, client.right, client.bottom);
        auto oldBmp = SelectObject(dc, bmp);
        const ui::Theme t = ui::CurrentTheme();
        const UINT dpi = Dpi();
        const Settings& s = g_settings;

        HBRUSH bg = CreateSolidBrush(t.bg);
        FillRect(dc, &client, bg);
        DeleteObject(bg);
        Fonts(dpi);
        HFONT title = g_fonts.title, body = g_fonts.body, note = g_fonts.note, icons = g_fonts.icons;
        auto of = SelectObject(dc, title);
        ui::Text(dc, L"WebRTC Audio Engine", {S(kPad), 0, S(kWidth - kPad), S(kTitleH)}, t.text);

        // Microphone picker + whose settings the switches change.
        ui::DrawDropdown(dc, MicRect(), MicName(), !g_mics.empty(), g_hot == F_MIC || ui::ListOpen(), body, icons, t, dpi);
        SelectObject(dc, note);
        ui::Text(dc, g_micGuid.empty() ? L"Switches change the default settings"
                     : Scope()         ? L"Custom settings for this microphone"
                                       : L"Uses the default settings",
                 {S(kPad + 2), S(kScopeTop), S(kWidth - kPad), S(kCardTop - 6)}, t.subtle);

        // Switch rows on a card: dot, name, status line, switch.
        ui::FillRound(dc, {S(kPad / 2), S(kCardTop), S(kWidth - kPad / 2), S(kCardTop + 16 + ROW_COUNT * kRowH)}, S(10), t.card);
        for (int row = 0; row < ROW_COUNT; ++row)
        {
            const RECT r = RowRect(row);
            std::wstring detail;
            const ui::Dot dot = RowState(s, row, detail);
            ui::DrawDot(dc, {r.left + S(10), (r.top + r.bottom) / 2}, dot, t, dpi);
            const RECT sw = ui::SwitchRect({r.left, r.top, r.right - S(6), r.bottom}, dpi);
            SelectObject(dc, body);
            ui::Text(dc, kRowNames[row], {r.left + S(26), r.top + S(6), sw.left - S(8), (r.top + r.bottom) / 2 + S(2)}, t.text,
                     DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, note);
            ui::Text(dc, detail, {r.left + S(26), (r.top + r.bottom) / 2 + S(2), sw.left - S(8), r.bottom - S(4)}, t.subtle,
                     DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            ui::DrawSwitch(dc, sw, RowOn(s, row), true, t, dpi);
            if (g_cues && g_focus == row) ui::DrawFocus(dc, sw, (sw.bottom - sw.top) / 2, t, dpi);
        }

        // "More settings" link
        SelectObject(dc, body);
        RECT link = LinkRect();
        ui::Text(dc, L"More settings\x2026", link, t.accent);
        if (g_cues && g_focus == F_LINK) ui::DrawFocus(dc, link, S(4), t, dpi);
        if (g_cues && g_focus == F_MIC && !ui::ListOpen()) ui::DrawFocus(dc, MicRect(), S(6), t, dpi);

        SelectObject(dc, of);
        BitBlt(wdc, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(hwnd, &ps);
    }

    // ---------------------------------------------------------------- actions

    void HideFlyout();

    void Toggle(int row)
    {
        Settings s = Settings::ReadFromRegistry(Scope());
        switch (row)
        {
        case ROW_AEC: s.aecEnabled = !s.aecEnabled; break;
        case ROW_NS: s.nsEnabled = !s.nsEnabled; break;
        case ROW_AGC: s.agcEnabled = !s.agcEnabled; break;
        case ROW_CLEANUP:
        {
            const bool on = s.hpfEnabled || s.lpfEnabled;
            s.hpfEnabled = s.lpfEnabled = on ? 0u : 1u;  // off <-> rumble + hiss
            break;
        }
        }
        if (Settings::WriteToRegistry(s, Scope()) != ERROR_SUCCESS)
            MessageBeep(MB_ICONWARNING);  // e.g. the settings key is missing or not writable
        ReloadSettings();
        InvalidateRect(g_flyout, nullptr, FALSE);
    }

    void PickMic(int i)
    {
        if (i < 0 || i >= static_cast<int>(g_mics.size())) return;
        g_micGuid = g_mics[static_cast<size_t>(i)].guid;
        SavePreferredMic(g_micGuid);
        ReloadSettings();
        g_speech = false;
        ReadLive();
        InvalidateRect(g_flyout, nullptr, FALSE);
    }

    void OpenMicList()
    {
        RefreshMics();
        if (g_mics.empty()) return;
        telemetry::Device live[telemetry::kMaxSlots];
        const int n = telemetry::ReadAll(live);
        std::vector<ui::ListItem> items;
        for (const app::Mic& m : g_mics)
        {
            bool inUse = false;
            for (int k = 0; k < n; ++k) inUse |= _wcsicmp(live[k].device, m.guid.c_str()) == 0;
            items.push_back({m.name, inUse ? L"in use" : !m.connected ? L"not connected" : L"", inUse});
        }
        RECT r = MicRect();
        MapWindowPoints(g_flyout, nullptr, reinterpret_cast<POINT*>(&r), 2);
        ui::ShowList(g_flyout, r, items, MicIndex(g_micGuid), [](int i) { PickMic(i); });
    }

    void OpenSettings()
    {
        const std::wstring guid = g_micGuid;
        HideFlyout();
        app::ShowMainWindow(guid.empty() ? nullptr : guid.c_str());
    }

    void ShowFlyout()
    {
        RefreshMics();
        ReloadSettings();
        ReadLive();
        g_focus = F_MIC;
        g_cues = false;
        // Place it above the tray icon (Windows 11 taskbar is usually at the bottom).
        const int w = S(kWidth), h = S(kHeight);
        NOTIFYICONIDENTIFIER id{sizeof(id), g_flyout, kTrayId};
        RECT icon{};
        POINT anchor{};
        if (SUCCEEDED(Shell_NotifyIconGetRect(&id, &icon)))
            anchor = {(icon.left + icon.right) / 2, icon.top};
        else
            GetCursorPos(&anchor);
        HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        const RECT wa = mi.rcWork;
        int x = std::clamp(static_cast<int>(anchor.x - w / 2), static_cast<int>(wa.left + S(12)), static_cast<int>(wa.right - w - S(12)));
        int y = anchor.y < (wa.top + wa.bottom) / 2 ? wa.top + S(12) : wa.bottom - h - S(12);
        SetWindowPos(g_flyout, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
        SetForegroundWindow(g_flyout);
        SetTimer(g_flyout, kMeterTimer, 100, nullptr);
        InvalidateRect(g_flyout, nullptr, FALSE);
    }

    void HideFlyout()
    {
        ui::CloseList();
        KillTimer(g_flyout, kMeterTimer);
        ShowWindow(g_flyout, SW_HIDE);
        g_hiddenAt = GetTickCount64();
    }

    void ContextMenu()
    {
        POINT pt;
        GetCursorPos(&pt);
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, CMD_OPEN, L"Open settings");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CMD_EXIT, L"Exit");
        SetForegroundWindow(g_flyout);
        const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_flyout, nullptr);
        DestroyMenu(m);
        if (cmd == CMD_OPEN) app::ShowMainWindow();
        else if (cmd == CMD_EXIT) app::ExitApp();
    }

    // Which control is at p: F_MIC, a row, F_LINK, or -2.
    int HitTest(POINT p)
    {
        RECT mic = MicRect();
        if (PtInRect(&mic, p)) return F_MIC;
        for (int row = 0; row < ROW_COUNT; ++row)
        {
            RECT r = RowRect(row);
            if (PtInRect(&r, p)) return row;
        }
        RECT link = LinkRect();
        return PtInRect(&link, p) ? F_LINK : -2;
    }

    void Activate(int what)
    {
        if (what == F_MIC) OpenMicList();
        else if (what == F_LINK) OpenSettings();
        else if (what >= 0 && what < ROW_COUNT) Toggle(what);
    }

    LRESULT CALLBACK FlyoutProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == g_taskbarCreated && g_taskbarCreated)
        {
            AddTrayIcon();  // Explorer restarted
            return 0;
        }
        switch (msg)
        {
        case WM_TRAY:
            // NOTIFYICON_VERSION_4: a left click arrives as NIN_SELECT (after WM_LBUTTONDOWN/UP, which we
            // ignore: handling both would open and immediately close the flyout). Keyboard: NIN_KEYSELECT.
            switch (LOWORD(lp))
            {
            case NIN_SELECT:
            case NIN_KEYSELECT:
                if (IsWindowVisible(hwnd)) HideFlyout();
                else if (GetTickCount64() - g_hiddenAt > 300) ShowFlyout();
                break;
            case WM_CONTEXTMENU:
                ContextMenu();
                break;
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && IsWindowVisible(hwnd))
                HideFlyout();
            return 0;
        case WM_KEYDOWN:
            if (ui::ListOpen()) { ui::ListKey(wp); return 0; }
            switch (wp)
            {
            case VK_ESCAPE: HideFlyout(); break;
            case VK_TAB:
            {
                const int dir = GetKeyState(VK_SHIFT) < 0 ? -1 : 1;
                g_focus = (g_focus + 1 + dir + ROW_COUNT + 2) % (ROW_COUNT + 2) - 1;  // F_MIC .. F_LINK
                g_cues = true;
                break;
            }
            case VK_SPACE:
            case VK_RETURN: g_cues = true; Activate(g_focus); break;
            case VK_UP:
            case VK_DOWN:
                g_cues = true;
                if (g_focus == F_MIC)
                {
                    RefreshMics();
                    PickMic(std::clamp(MicIndex(g_micGuid) + (wp == VK_DOWN ? 1 : -1), 0, static_cast<int>(g_mics.size()) - 1));
                }
                break;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_TIMER:
        {
            ReadLive();
            std::wstring view = LiveView();
            if (view != g_shown)  // nothing visible changed: no repaint
            {
                g_shown = std::move(view);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
        {
            g_cues = false;
            const int what = HitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (what != -2) { g_focus = what; Activate(what); }
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            const int what = HitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (what != g_hot)
            {
                g_hot = what;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_hot = -2;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT)
            {
                SetCursor(LoadCursorW(nullptr, g_hot != -2 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_DPICHANGED:
            AddTrayIcon();  // redraw the icon for the new theme / DPI (NIM_ADD on an existing id just fails)
            g_nid.uFlags = NIF_ICON;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            ui::ApplyWindowTheme(hwnd, ui::CurrentTheme());
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            Paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

namespace tray
{
    bool Init(HINSTANCE inst)
    {
        g_inst = inst;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = FlyoutProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"WebrtcAudioEngineFlyout";
        RegisterClassExW(&wc);
        g_flyout = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"WebRTC Audio Engine", WS_POPUP, 0, 0,
                                   10, 10, nullptr, nullptr, inst, nullptr);
        if (!g_flyout)
            return false;
        ui::ApplyWindowTheme(g_flyout, ui::CurrentTheme());  // Windows 11 rounded corners + shadow, dark frame
        g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        // When the app runs elevated (e.g. started from an admin shell), Windows blocks messages from the
        // non-elevated taskbar (UIPI): allow the tray callback and Explorer's restart broadcast explicitly.
        ChangeWindowMessageFilterEx(g_flyout, WM_TRAY, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(g_flyout, g_taskbarCreated, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(g_flyout, WM_COMMAND, MSGFLT_ALLOW, nullptr);
        AddTrayIcon();
        return true;
    }

    void Shutdown()
    {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_icon) DestroyIcon(g_icon);
        if (g_flyout) DestroyWindow(g_flyout);
        for (HFONT f : {g_fonts.title, g_fonts.body, g_fonts.note, g_fonts.icons})
            if (f) DeleteObject(f);
        g_fonts = {};
        g_icon = nullptr;
        g_flyout = nullptr;
    }

    void SettingsChanged()
    {
        if (g_flyout && IsWindowVisible(g_flyout))
        {
            ReloadSettings();
            InvalidateRect(g_flyout, nullptr, FALSE);
        }
    }
}
