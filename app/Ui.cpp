// SPDX-License-Identifier: GPL-3.0-or-later
#include "Ui.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#define DWMWA_TEXT_COLOR 36
#endif

namespace ui
{
    bool IsDark()
    {
        DWORD v = 1, size = sizeof(v);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &v, &size);
        return v == 0;
    }

    Theme CurrentTheme()
    {
        DWORD color = 0;
        BOOL opaque = FALSE;
        COLORREF accent = RGB(0, 120, 212);
        if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque)))
            accent = RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
        Theme t{};
        t.dark = IsDark();
        t.accent = accent;
        t.onAccent = RGB(255, 255, 255);
        t.dotHold = RGB(255, 185, 0);   // amber
        t.dotAlert = RGB(232, 72, 85);  // red
        if (t.dark)
        {
            t.bg = RGB(32, 32, 32); t.card = RGB(43, 43, 43); t.control = RGB(56, 56, 56); t.controlHover = RGB(64, 64, 64);
            t.line = RGB(64, 64, 64); t.text = RGB(240, 240, 240); t.subtle = RGB(160, 160, 160); t.disabled = RGB(105, 105, 105);
            t.track = RGB(90, 90, 90); t.dotLit = RGB(245, 245, 245); t.dotOff = RGB(90, 90, 90);
        }
        else
        {
            t.bg = RGB(243, 243, 243); t.card = RGB(255, 255, 255); t.control = RGB(240, 240, 240); t.controlHover = RGB(232, 232, 232);
            t.line = RGB(225, 225, 225); t.text = RGB(26, 26, 26); t.subtle = RGB(96, 96, 96); t.disabled = RGB(160, 160, 160);
            t.track = RGB(160, 160, 160); t.dotLit = RGB(32, 32, 32); t.dotOff = RGB(190, 190, 190);
        }
        return t;
    }

    HFONT Font(int pt, int weight, UINT dpi)
    {
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(pt, static_cast<int>(dpi), 72);
        lf.lfWeight = weight;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI Variable Text");
        return CreateFontIndirectW(&lf);
    }

    HFONT IconFont(int pt, UINT dpi)
    {
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(pt, static_cast<int>(dpi), 72);
        lf.lfQuality = ANTIALIASED_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe Fluent Icons");
        return CreateFontIndirectW(&lf);
    }

    // Renders a signed-distance shape into a premultiplied 32-bit bitmap and blends it onto `dc`.
    template <class Distance>
    void BlendShape(HDC dc, const RECT& r, COLORREF c, Distance dist)
    {
        const int w = r.right - r.left, h = r.bottom - r.top;
        if (w <= 0 || h <= 0) return;
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits)
        {
            if (bmp) DeleteObject(bmp);
            return;
        }
        auto* px = static_cast<uint32_t*>(bits);
        const uint32_t cr = GetRValue(c), cg = GetGValue(c), cb = GetBValue(c);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const float a = std::clamp(0.5f - dist(x + 0.5f, y + 0.5f), 0.0f, 1.0f);
                const uint32_t A = static_cast<uint32_t>(a * 255.0f + 0.5f);
                px[y * w + x] = (A << 24) | ((cr * A / 255) << 16) | ((cg * A / 255) << 8) | (cb * A / 255);
            }
        HDC mem = CreateCompatibleDC(dc);
        auto old = SelectObject(mem, bmp);
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        AlphaBlend(dc, r.left, r.top, w, h, mem, 0, 0, w, h, bf);
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(bmp);
    }

    void FillRound(HDC dc, const RECT& r, int radius, COLORREF c, float stroke)
    {
        const float w = static_cast<float>(r.right - r.left), h = static_cast<float>(r.bottom - r.top);
        const float rad = std::min({static_cast<float>(radius), w / 2, h / 2});
        BlendShape(dc, r, c, [&](float x, float y) {
            const float qx = std::max(std::abs(x - w / 2) - (w / 2 - rad), 0.0f);
            const float qy = std::max(std::abs(y - h / 2) - (h / 2 - rad), 0.0f);
            const float inside = std::min(std::max(std::abs(x - w / 2) - (w / 2 - rad), std::abs(y - h / 2) - (h / 2 - rad)), 0.0f);
            const float d = std::hypot(qx, qy) + inside - rad;
            return stroke > 0 ? std::abs(d + stroke / 2) - stroke / 2 : d;
        });
    }

    void FillCircle(HDC dc, POINT c, int radius, COLORREF color, float stroke)
    {
        const RECT r{c.x - radius - 1, c.y - radius - 1, c.x + radius + 1, c.y + radius + 1};
        const float cx = radius + 1.0f, cy = radius + 1.0f, rad = static_cast<float>(radius);
        BlendShape(dc, r, color, [&](float x, float y) {
            const float d = std::hypot(x - cx, y - cy) - rad;
            return stroke > 0 ? std::abs(d + stroke / 2) - stroke / 2 : d;
        });
    }

    void Text(HDC dc, const std::wstring& s, RECT r, COLORREF c, UINT format)
    {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, c);
        DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, format | DT_NOPREFIX);
    }

    RECT SwitchRect(const RECT& row, UINT dpi)
    {
        const int w = Px(40, dpi), h = Px(20, dpi);
        return {row.right - w, (row.top + row.bottom - h) / 2, row.right, (row.top + row.bottom - h) / 2 + h};
    }

    void DrawSwitch(HDC dc, const RECT& r, bool on, bool enabled, const Theme& t, UINT dpi)
    {
        const int h = r.bottom - r.top;
        const COLORREF fill = !enabled ? t.control : on ? t.accent : t.card;
        FillRound(dc, r, h, fill);
        if (!on || !enabled)
            FillRound(dc, r, h, enabled ? t.subtle : t.disabled, static_cast<float>(Px(1, dpi)));  // outlined when off
        const int knob = on ? h - Px(8, dpi) : h - Px(10, dpi);
        const int cx = on ? r.right - h / 2 : r.left + h / 2, cy = (r.top + r.bottom) / 2;
        FillCircle(dc, {cx, cy}, knob / 2, !enabled ? t.disabled : on ? t.onAccent : t.subtle);
    }

    void DrawSegmented(HDC dc, const RECT& r, const std::vector<std::wstring>& labels, int selected, bool enabled, bool multi,
                       HFONT font, const Theme& t, UINT dpi)
    {
        const int n = static_cast<int>(labels.size());
        if (n == 0) return;
        const int radius = Px(6, dpi), inset = Px(2, dpi);
        FillRound(dc, r, radius, t.control);
        auto old = SelectObject(dc, font);
        const int w = r.right - r.left;
        for (int i = 0; i < n; ++i)
        {
            RECT seg{r.left + w * i / n, r.top, r.left + w * (i + 1) / n, r.bottom};
            const bool on = multi ? ((selected >> i) & 1) != 0 : i == selected;
            if (on)
            {
                RECT fill{seg.left + inset, seg.top + inset, seg.right - inset, seg.bottom - inset};
                FillRound(dc, fill, radius - inset, enabled ? t.accent : t.track);
            }
            else if (i > 0 && !(multi ? ((selected >> (i - 1)) & 1) : i - 1 == selected))
            {
                RECT sep{seg.left, seg.top + Px(8, dpi), seg.left + 1, seg.bottom - Px(8, dpi)};  // divider between plain segments
                HBRUSH b = CreateSolidBrush(t.line);
                FillRect(dc, &sep, b);
                DeleteObject(b);
            }
            Text(dc, labels[static_cast<size_t>(i)], seg, !enabled ? t.disabled : on ? t.onAccent : t.text,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        SelectObject(dc, old);
    }

    int SegmentAt(const RECT& r, int count, POINT p)
    {
        if (count <= 0 || !PtInRect(&r, p)) return -1;
        return std::clamp(static_cast<int>((p.x - r.left) * count / std::max(1L, r.right - r.left)), 0, count - 1);
    }

    void DrawDropdown(HDC dc, const RECT& r, const std::wstring& text, bool enabled, bool hot, HFONT font, HFONT icons,
                      const Theme& t, UINT dpi)
    {
        FillRound(dc, r, Px(6, dpi), hot && enabled ? t.controlHover : t.control);
        auto old = SelectObject(dc, font);
        RECT tr{r.left + Px(12, dpi), r.top, r.right - Px(36, dpi), r.bottom};
        Text(dc, text, tr, enabled ? t.text : t.disabled);
        SelectObject(dc, icons);
        RECT ir{r.right - Px(32, dpi), r.top, r.right - Px(8, dpi), r.bottom};
        Text(dc, L"\xE70D", ir, enabled ? t.subtle : t.disabled, DT_CENTER | DT_VCENTER | DT_SINGLELINE);  // ChevronDown
        SelectObject(dc, old);
    }

    void DrawFocus(HDC dc, const RECT& r, int radius, const Theme& t, UINT dpi)
    {
        const int g = Px(3, dpi);
        RECT f{r.left - g, r.top - g, r.right + g, r.bottom + g};
        FillRound(dc, f, radius + g, t.text, static_cast<float>(Px(2, dpi)));
    }

    void DrawDot(HDC dc, POINT c, Dot d, const Theme& t, UINT dpi)
    {
        const int rad = Px(5, dpi);
        switch (d)
        {
        case Dot::Lit: FillCircle(dc, c, rad, t.dotLit); break;
        case Dot::Hold: FillCircle(dc, c, rad, t.dotHold); break;
        case Dot::Alert: FillCircle(dc, c, rad, t.dotAlert); break;
        case Dot::Off: FillCircle(dc, c, rad, t.dotOff); break;
        case Dot::Idle: FillCircle(dc, c, rad, t.subtle, static_cast<float>(Px(1, dpi)) + 0.5f); break;
        }
    }

    void ApplyWindowTheme(HWND hwnd, const Theme& t, bool roundCorners)
    {
        const BOOL dark = t.dark;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        const COLORREF caption = t.bg, text = t.text;  // Windows 11 22000+; ignored where unsupported
        DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
        if (roundCorners)
        {
            const DWM_WINDOW_CORNER_PREFERENCE round = DWMWCP_ROUND;
            DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &round, sizeof(round));
        }
    }

    // ------------------------------------------------------------------------------------ list popup

    namespace
    {
        HWND g_list = nullptr, g_listOwner = nullptr;
        std::vector<ListItem> g_items;
        int g_listSel = -1, g_listHot = -1;
        std::function<void(int)> g_onPick;
        constexpr int kItemH = 36, kListPad = 4;

        UINT ListDpi() { return GetDpiForWindow(g_listOwner ? g_listOwner : g_list); }

        void Finish(int pick)
        {
            if (!g_list) return;
            HWND w = g_list;
            auto cb = std::move(g_onPick);
            g_list = nullptr;
            g_onPick = nullptr;
            if (GetCapture() == w) ReleaseCapture();
            DestroyWindow(w);
            if (pick >= 0 && cb) cb(pick);
            if (g_listOwner) InvalidateRect(g_listOwner, nullptr, FALSE);
        }

        int ItemAt(POINT p)
        {
            const UINT dpi = ListDpi();
            if (p.y < Px(kListPad, dpi)) return -1;
            const int i = (p.y - Px(kListPad, dpi)) / Px(kItemH, dpi);
            RECT c;
            GetClientRect(g_list, &c);
            return (p.x >= 0 && p.x < c.right && i >= 0 && i < static_cast<int>(g_items.size())) ? i : -1;
        }

        void PaintList(HWND hwnd)
        {
            PAINTSTRUCT ps;
            HDC wdc = BeginPaint(hwnd, &ps);
            RECT c;
            GetClientRect(hwnd, &c);
            HDC dc = CreateCompatibleDC(wdc);
            HBITMAP bmp = CreateCompatibleBitmap(wdc, c.right, c.bottom);
            auto oldBmp = SelectObject(dc, bmp);
            const Theme t = CurrentTheme();
            const UINT dpi = ListDpi();
            HBRUSH bg = CreateSolidBrush(t.card);
            FillRect(dc, &c, bg);
            DeleteObject(bg);
            HFONT body = Font(10, FW_NORMAL, dpi), smallFont = Font(9, FW_NORMAL, dpi);
            for (int i = 0; i < static_cast<int>(g_items.size()); ++i)
            {
                const ListItem& it = g_items[static_cast<size_t>(i)];
                RECT r{Px(kListPad, dpi), Px(kListPad + i * kItemH, dpi), c.right - Px(kListPad, dpi), Px(kListPad + (i + 1) * kItemH, dpi)};
                if (i == g_listHot || i == g_listSel)
                    FillRound(dc, r, Px(4, dpi), i == g_listHot ? t.controlHover : t.control);
                if (i == g_listSel)  // Windows 11 style selection pill
                {
                    RECT pill{r.left, r.top + Px(10, dpi), r.left + Px(3, dpi), r.bottom - Px(10, dpi)};
                    FillRound(dc, pill, Px(2, dpi), t.accent);
                }
                int right = r.right - Px(10, dpi);
                if (!it.detail.empty())
                {
                    SelectObject(dc, smallFont);
                    SIZE sz{};
                    GetTextExtentPoint32W(dc, it.detail.c_str(), static_cast<int>(it.detail.size()), &sz);
                    RECT dr{right - sz.cx, r.top, right, r.bottom};
                    Text(dc, it.detail, dr, t.subtle, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    right -= sz.cx + Px(6, dpi);
                    if (it.marked)
                    {
                        FillCircle(dc, {right - Px(4, dpi), (r.top + r.bottom) / 2}, Px(3, dpi), t.dotLit);
                        right -= Px(14, dpi);
                    }
                }
                SelectObject(dc, body);
                RECT tr{r.left + Px(12, dpi), r.top, right, r.bottom};
                Text(dc, it.text, tr, t.text);
            }
            DeleteObject(body);
            DeleteObject(smallFont);
            BitBlt(wdc, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
            SelectObject(dc, oldBmp);
            DeleteObject(bmp);
            DeleteDC(dc);
            EndPaint(hwnd, &ps);
        }

        LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            switch (msg)
            {
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            case WM_MOUSEMOVE:
            {
                const int hot = ItemAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                if (hot != g_listHot) { g_listHot = hot; InvalidateRect(hwnd, nullptr, FALSE); }
                return 0;
            }
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            {
                // With capture, clicks anywhere arrive here: outside the list cancels it.
                RECT c;
                GetClientRect(hwnd, &c);
                const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (!PtInRect(&c, p)) Finish(-1);
                return 0;
            }
            case WM_LBUTTONUP:
            {
                const int i = ItemAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                if (i >= 0) Finish(i);
                return 0;
            }
            case WM_CAPTURECHANGED:
                if (g_list == hwnd && reinterpret_cast<HWND>(lp) != hwnd) Finish(-1);
                return 0;
            case WM_PAINT:
                PaintList(hwnd);
                return 0;
            case WM_ERASEBKGND:
                return 1;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    void ShowList(HWND owner, const RECT& anchor, const std::vector<ListItem>& items, int selected, std::function<void(int)> onPick)
    {
        CloseList();
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = ListProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = L"WebrtcAudioEngineList";
            wc.style = CS_DROPSHADOW;
            registered = RegisterClassExW(&wc) != 0;
        }
        g_listOwner = owner;
        g_items = items;
        g_listSel = selected;
        g_listHot = -1;
        g_onPick = std::move(onPick);
        const UINT dpi = GetDpiForWindow(owner);
        const int w = anchor.right - anchor.left;
        const int h = Px(2 * kListPad + kItemH * static_cast<int>(items.size()), dpi);
        // Below the field, or above it when there is no room on this monitor.
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &mi);
        int y = anchor.bottom + Px(4, dpi);
        if (y + h > mi.rcWork.bottom) y = anchor.top - Px(4, dpi) - h;
        g_list = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"WebrtcAudioEngineList", L"", WS_POPUP,
                                 anchor.left, y, w, h, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!g_list) return;
        ApplyWindowTheme(g_list, CurrentTheme());
        ShowWindow(g_list, SW_SHOWNOACTIVATE);
        SetCapture(g_list);
    }

    bool ListOpen() { return g_list != nullptr; }
    void CloseList() { Finish(-1); }

    bool ListKey(WPARAM vk)
    {
        if (!g_list) return false;
        const int n = static_cast<int>(g_items.size());
        int& cur = g_listHot >= 0 ? g_listHot : g_listSel;
        int next = cur;
        switch (vk)
        {
        case VK_UP: next = std::max(0, cur - 1); break;
        case VK_DOWN: next = std::min(n - 1, cur + 1); break;
        case VK_HOME: next = 0; break;
        case VK_END: next = n - 1; break;
        case VK_RETURN:
        case VK_SPACE: Finish(cur); return true;
        case VK_ESCAPE:
        case VK_TAB: Finish(-1); return true;
        default: return true;  // swallow other keys while open
        }
        g_listHot = next;
        InvalidateRect(g_list, nullptr, FALSE);
        return true;
    }
}
