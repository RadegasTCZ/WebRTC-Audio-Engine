// SPDX-License-Identifier: GPL-3.0-or-later
// Tray icon + a fontSmall home-brew flyout: quick switches for echo cancellation, noise suppression,
// automatic gain and cleanup, plus live meters (echo removed, gain) from the audio effect.
// Everything is drawn with plain GDI and follows the Windows light/dark app theme.
#include "App.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <string>
#include "../src/Settings.h"
#include "../src/Telemetry.h"

namespace
{
    constexpr UINT WM_TRAY = WM_APP + 1;
    constexpr UINT kTrayId = 1;
    constexpr UINT_PTR kMeterTimer = 1;
    enum MenuCmd : UINT { CMD_OPEN = 1, CMD_EXIT };

    HINSTANCE g_inst = nullptr;
    HWND g_flyout = nullptr;
    HICON g_icon = nullptr;
    NOTIFYICONDATAW g_nid{};
    UINT g_taskbarCreated = 0;
    ULONGLONG g_hiddenAt = 0;  // a click on the tray icon right after the flyout lost focus should not reopen it

    // ---------------------------------------------------------------- theme

    struct Theme
    {
        COLORREF bg, card, text, subtle, accent, track, meterBg;
    };
    bool IsDark()
    {
        DWORD v = 1, size = sizeof(v);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
        return v == 0;
    }
    Theme CurrentTheme()
    {
        DWORD color = 0;
        BOOL opaque = FALSE;
        COLORREF accent = RGB(0, 120, 212);
        if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque)))
            accent = RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
        if (IsDark())
            return {RGB(32, 32, 32), RGB(43, 43, 43), RGB(240, 240, 240), RGB(160, 160, 160), accent, RGB(90, 90, 90), RGB(60, 60, 60)};
        return {RGB(243, 243, 243), RGB(255, 255, 255), RGB(26, 26, 26), RGB(96, 96, 96), accent, RGB(160, 160, 160), RGB(225, 225, 225)};
    }

    // ---------------------------------------------------------------- tray icon (drawn at runtime)

    HICON MakeIcon(int size)
    {
        // A home-brew glyph: five rounded "level" bars, like a voice waveform.
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
        auto* px = static_cast<uint32_t*>(bits);
        std::fill(px, px + size * size, 0u);
        const uint32_t ink = IsDark() ? 0xFFFFFFFF : 0xFF202020;
        const float heights[5] = {0.35f, 0.7f, 1.0f, 0.6f, 0.3f};
        const float barW = size / 8.0f, gap = size / 16.0f;
        const float total = 5 * barW + 4 * gap, x0 = (size - total) / 2.0f;
        for (int b = 0; b < 5; ++b)
        {
            const float h = heights[b] * size * 0.85f, top = (size - h) / 2.0f;
            const float xa = x0 + b * (barW + gap);
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                {
                    const float cx = x + 0.5f, cy = y + 0.5f;
                    // Rounded bar: distance to the bar's centre segment.
                    const float mx = std::clamp(cx, xa + barW / 2, xa + barW / 2);
                    const float my = std::clamp(cy, top + barW / 2, top + h - barW / 2);
                    const float d = std::hypot(cx - mx, cy - my) - barW / 2;
                    const float a = std::clamp(0.5f - d, 0.0f, 1.0f);
                    if (a > 0)
                    {
                        const uint32_t alpha = static_cast<uint32_t>(a * 255);
                        const uint32_t c = ink & 0x00FFFFFF;
                        // premultiplied
                        const uint32_t r = ((c >> 16) & 0xFF) * alpha / 255, g = ((c >> 8) & 0xFF) * alpha / 255, bl = (c & 0xFF) * alpha / 255;
                        px[y * size + x] = (alpha << 24) | (r << 16) | (g << 8) | bl;
                    }
                }
        }
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

    // ---------------------------------------------------------------- flyout layout + drawing

    enum Row { ROW_AEC, ROW_NS, ROW_AGC, ROW_CLEANUP, ROW_COUNT };
    const wchar_t* kRowNames[ROW_COUNT] = {L"Echo cancellation", L"Noise suppression", L"Automatic gain", L"Cleanup filters"};

    int Dpi() { return static_cast<int>(GetDpiForWindow(g_flyout)); }
    int S(int v) { return MulDiv(v, Dpi(), 96); }
    constexpr int kWidth = 300, kPad = 14, kTitleH = 40, kRowH = 36, kMeterH = 30, kLinkH = 40;
    int HeightDip() { return kTitleH + ROW_COUNT * kRowH + 10 + 2 * kMeterH + 24 + kLinkH; }

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
    RECT RowRect(int row) { return {S(kPad), S(kTitleH + row * kRowH), S(kWidth - kPad), S(kTitleH + (row + 1) * kRowH)}; }
    RECT LinkRect() { const int top = HeightDip() - kLinkH; return {S(kPad), S(top + 6), S(kWidth - kPad), S(HeightDip() - 6)}; }

    HFONT Font(int pt, int weight)
    {
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(pt, Dpi(), 72);
        lf.lfWeight = weight;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI Variable Text");
        return CreateFontIndirectW(&lf);
    }

    void FillRound(HDC dc, RECT r, int radius, COLORREF c)
    {
        HBRUSH b = CreateSolidBrush(c);
        HPEN p = CreatePen(PS_SOLID, 1, c);
        auto ob = SelectObject(dc, b);
        auto op = SelectObject(dc, p);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(b);
        DeleteObject(p);
    }

    void DrawSwitch(HDC dc, RECT row, bool on, const Theme& t)
    {
        const int w = S(40), h = S(20);
        RECT r{row.right - w, (row.top + row.bottom - h) / 2, row.right, (row.top + row.bottom + h) / 2};
        FillRound(dc, r, h, on ? t.accent : t.track);
        const int knob = h - S(8);
        const int kx = on ? r.right - S(4) - knob : r.left + S(4);
        RECT k{kx, r.top + S(4), kx + knob, r.top + S(4) + knob};
        FillRound(dc, k, knob, on ? RGB(255, 255, 255) : t.card);
    }

    void DrawMeter(HDC dc, int top, const wchar_t* label, float value, float maxValue, const std::wstring& text, const Theme& t,
                   bool valid)
    {
        RECT lab{S(kPad), S(top), S(kPad + 110), S(top + kMeterH)};
        SetTextColor(dc, t.subtle);
        DrawTextW(dc, label, -1, &lab, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT bar{S(kPad + 112), S(top + 11), S(kWidth - kPad - 64), S(top + 19)};
        FillRound(dc, bar, S(8), t.meterBg);
        if (valid && value > 0)
        {
            RECT fill = bar;
            fill.right = bar.left + static_cast<int>((bar.right - bar.left) * std::clamp(value / maxValue, 0.0f, 1.0f));
            if (fill.right - fill.left >= S(8)) FillRound(dc, fill, S(8), t.accent);
        }
        RECT val{S(kWidth - kPad - 60), S(top), S(kWidth - kPad), S(top + kMeterH)};
        SetTextColor(dc, valid ? t.text : t.subtle);
        DrawTextW(dc, valid ? text.c_str() : L"-", -1, &val, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
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
        const Theme t = CurrentTheme();
        const Settings s = Settings::ReadFromRegistry();

        HBRUSH bg = CreateSolidBrush(t.bg);
        FillRect(dc, &client, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        HFONT title = Font(11, FW_SEMIBOLD), body = Font(10, FW_NORMAL), fontSmall = Font(9, FW_NORMAL);

        // Title
        auto of = SelectObject(dc, title);
        SetTextColor(dc, t.text);
        RECT tr{S(kPad), 0, S(kWidth - kPad), S(kTitleH)};
        DrawTextW(dc, L"WebRTC Audio Engine", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Switch rows on a card
        RECT card{S(kPad / 2), S(kTitleH - 4), S(kWidth - kPad / 2), S(kTitleH + ROW_COUNT * kRowH + 4)};
        FillRound(dc, card, S(10), t.card);
        SelectObject(dc, body);
        for (int row = 0; row < ROW_COUNT; ++row)
        {
            RECT r = RowRect(row);
            RECT text = r;
            text.left += S(6);
            SetTextColor(dc, t.text);
            DrawTextW(dc, kRowNames[row], -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            r.right -= S(6);
            DrawSwitch(dc, r, RowOn(s, row), t);
        }

        // Live meters
        telemetry::Data d{};
        const bool live = telemetry::Read(d, 1500);
        int y = kTitleH + ROW_COUNT * kRowH + 14;
        SelectObject(dc, fontSmall);
        const bool echoValid = live && d.aecRunning && s.aecEnabled;
        wchar_t buf[32];
        swprintf_s(buf, L"%.0f dB", std::max(0.0f, static_cast<float>(d.echoRemovedDb)));
        DrawMeter(dc, y, L"Echo removed", d.echoRemovedDb, 40.0f, buf, t, echoValid);
        y += kMeterH;
        swprintf_s(buf, L"%+.0f dB", static_cast<float>(d.agcGainDb));
        DrawMeter(dc, y, L"Automatic gain", d.agcGainDb, 30.0f, buf, t, live && s.agcEnabled);
        y += kMeterH;
        RECT status{S(kPad), S(y), S(kWidth - kPad), S(y + 20)};
        SetTextColor(dc, t.subtle);
        const wchar_t* msg = !live                     ? L"No app is using the microphone right now."
                             : !d.aecRunning           ? L"This recording has no echo reference."
                             : d.referenceDbfs < -70.0f ? L"Echo cancellation ready (nothing playing)."
                                                        : L"Removing echo from your speakers.";
        DrawTextW(dc, msg, -1, &status, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // "More settings" link
        SelectObject(dc, body);
        RECT link = LinkRect();
        SetTextColor(dc, t.accent);
        DrawTextW(dc, L"More settings\x2026", -1, &link, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(dc, of);
        DeleteObject(title);
        DeleteObject(body);
        DeleteObject(fontSmall);
        BitBlt(wdc, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(hwnd, &ps);
    }

    void Toggle(int row)
    {
        Settings s = Settings::ReadFromRegistry();
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
        Settings::WriteToRegistry(s);
        InvalidateRect(g_flyout, nullptr, FALSE);
    }

    void ShowFlyout()
    {
        // Place it above the tray icon (Windows 11 taskbar is usually at the bottom).
        const int w = S(kWidth), h = S(HeightDip());
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
            switch (LOWORD(lp))
            {
            case NIN_SELECT:
            case NIN_KEYSELECT:
            case WM_LBUTTONUP:
                if (IsWindowVisible(hwnd)) HideFlyout();
                else if (GetTickCount64() - g_hiddenAt > 250) ShowFlyout();
                break;
            case WM_CONTEXTMENU:
            case WM_RBUTTONUP:
                ContextMenu();
                break;
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && IsWindowVisible(hwnd)) HideFlyout();
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) HideFlyout();
            return 0;
        case WM_TIMER:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP:
        {
            const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            for (int row = 0; row < ROW_COUNT; ++row)
            {
                RECT r = RowRect(row);
                if (PtInRect(&r, p)) { Toggle(row); return 0; }
            }
            RECT link = LinkRect();
            if (PtInRect(&link, p))
            {
                HideFlyout();
                app::ShowMainWindow();
            }
            return 0;
        }
        case WM_SETCURSOR:
        {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            RECT link = LinkRect();
            bool hand = PtInRect(&link, p) != FALSE;
            for (int row = 0; row < ROW_COUNT && !hand; ++row) { RECT r = RowRect(row); hand = PtInRect(&r, p) != FALSE; }
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_DPICHANGED:
            AddTrayIcon();  // redraw the icon for the new theme / DPI (NIM_ADD on an existing id just fails)
            g_nid.uFlags = NIF_ICON;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
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
        const DWM_WINDOW_CORNER_PREFERENCE round = DWMWCP_ROUND;  // Windows 11 rounded corners + shadow
        DwmSetWindowAttribute(g_flyout, DWMWA_WINDOW_CORNER_PREFERENCE, &round, sizeof(round));
        const BOOL dark = IsDark();
        DwmSetWindowAttribute(g_flyout, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        AddTrayIcon();
        return true;
    }

    void Shutdown()
    {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_icon) DestroyIcon(g_icon);
        if (g_flyout) DestroyWindow(g_flyout);
    }

    void SettingsChanged()
    {
        if (g_flyout && IsWindowVisible(g_flyout))
            InvalidateRect(g_flyout, nullptr, FALSE);
    }
}
