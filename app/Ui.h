// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Small drawing kit shared by the settings window and the tray flyout, so both look the same and
// follow the Windows light/dark app theme: colors, smooth (anti-aliased) shapes, a switch, a segmented
// choice, a drop-down field, status dots, and a dark-aware drop-down list (popup).
// Plain GDI; shapes are rendered into small 32-bit bitmaps and alpha-blended.
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace ui
{
    struct Theme
    {
        bool dark;
        COLORREF bg, card, control, controlHover, line, text, subtle, disabled, accent, onAccent, track;
        COLORREF dotLit, dotHold, dotOff, dotAlert;
    };
    bool IsDark();
    Theme CurrentTheme();

    // Points to pixels at `dpi` (96 = 100 %).
    inline int Px(int dip, UINT dpi) { return MulDiv(dip, static_cast<int>(dpi), 96); }
    HFONT Font(int pt, int weight, UINT dpi);
    HFONT IconFont(int pt, UINT dpi);   // Segoe Fluent Icons (Windows 11)

    // Anti-aliased rounded rectangle; stroke > 0 draws only an outline of that width (pixels).
    void FillRound(HDC dc, const RECT& r, int radius, COLORREF c, float stroke = 0.0f);
    void FillCircle(HDC dc, POINT center, int radius, COLORREF c, float stroke = 0.0f);
    void Text(HDC dc, const std::wstring& s, RECT r, COLORREF c, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Controls (drawn; hit testing is up to the caller). `r` is the control's own rectangle.
    RECT SwitchRect(const RECT& row, UINT dpi);  // right-aligned in a row, vertically centred
    void DrawSwitch(HDC dc, const RECT& r, bool on, bool enabled, const Theme& t, UINT dpi);
    // Segmented choice; multi = independent chips (bit i of `selected` = segment i on).
    void DrawSegmented(HDC dc, const RECT& r, const std::vector<std::wstring>& labels, int selected, bool enabled, bool multi,
                       HFONT font, const Theme& t, UINT dpi);
    int SegmentAt(const RECT& r, int count, POINT p);  // -1 = none
    void DrawDropdown(HDC dc, const RECT& r, const std::wstring& text, bool enabled, bool hot, HFONT font, HFONT icons,
                      const Theme& t, UINT dpi);
    void DrawFocus(HDC dc, const RECT& r, int radius, const Theme& t, UINT dpi);

    // Activity dot: lit = working, hold = waiting/holding (amber), off = switched off (filled grey),
    // idle = this microphone is not recording (hollow ring), alert = limiter cutting (red).
    enum class Dot { Lit, Hold, Off, Idle, Alert };
    void DrawDot(HDC dc, POINT center, Dot d, const Theme& t, UINT dpi);

    // Dark title bar + caption color matching the background (Windows 11), rounded corners.
    void ApplyWindowTheme(HWND hwnd, const Theme& t, bool roundCorners = true);

    // ---- drop-down list: a non-activating popup under `anchor` (screen coordinates) ----
    struct ListItem
    {
        std::wstring text;
        std::wstring detail;   // small text on the right (e.g. "in use")
        bool marked = false;   // draws a small dot before the detail
    };
    // onPick(index) runs when an item is chosen; nothing happens on cancel. One list at a time.
    void ShowList(HWND owner, const RECT& anchor, const std::vector<ListItem>& items, int selected, std::function<void(int)> onPick);
    bool ListOpen();
    void CloseList();
    // While a list is open the owner forwards WM_KEYDOWN here (Up/Down/Home/End/Enter/Space/Escape).
    bool ListKey(WPARAM vk);
}
