// SPDX-License-Identifier: GPL-3.0-or-later
// WebRTC Audio Engine Settings: a small window over HKLM\SOFTWARE\WebrtcVoiceApo.
// Every change is written immediately; the audio effect picks it up live (within ~10 ms).
// The window also refreshes itself when the settings change elsewhere (scripts, another instance).
// Drawn with the shared kit (Ui.h) so it matches the tray flyout and follows the light/dark theme.
// Keyboard: Tab / Shift+Tab move between controls, Space / Enter switch or open, arrows pick choices.
#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <algorithm>
#include <array>
#include <string>
#include <vector>
#include "../src/Settings.h"
#include "../src/Telemetry.h"
#include "App.h"
#include "Ui.h"

// ------------------------------------------------------------------------------------ microphones

namespace
{
    constexpr wchar_t kOurPack[] = L"{9879C461-0BA4-4A7B-9F35-3C04EAC1772D}";
    constexpr PROPERTYKEY kSelectedPack = {{0xc876062a, 0xa276, 0x4ed6, {0xb1, 0x5b, 0x39, 0x62, 0xe6, 0x90, 0x07, 0xf8}}, 2};

    std::wstring GuidOf(const wchar_t* endpointId)
    {
        const wchar_t* brace = wcsrchr(endpointId, L'{');
        return brace ? brace : endpointId;
    }
}

std::vector<app::Mic> app::FindMics()
{
    std::vector<Mic> mics;
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&en))))
    {
        IMMDeviceCollection* col = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col)))
        {
            UINT n = 0;
            col->GetCount(&n);
            for (UINT i = 0; i < n; ++i)
            {
                IMMDevice* dev = nullptr;
                IPropertyStore* ps = nullptr;
                LPWSTR id = nullptr;
                if (SUCCEEDED(col->Item(i, &dev)) && SUCCEEDED(dev->GetId(&id)) && SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)))
                {
                    PROPVARIANT pack, name;
                    PropVariantInit(&pack);
                    PropVariantInit(&name);
                    ps->GetValue(kSelectedPack, &pack);
                    ps->GetValue(PKEY_Device_FriendlyName, &name);
                    if (pack.vt == VT_LPWSTR && _wcsicmp(pack.pwszVal, kOurPack) == 0)
                        mics.push_back({GuidOf(id), name.vt == VT_LPWSTR ? name.pwszVal : L"Microphone", true});
                    PropVariantClear(&pack);
                    PropVariantClear(&name);
                }
                if (ps) ps->Release();
                if (id) CoTaskMemFree(id);
                if (dev) dev->Release();
            }
            col->Release();
        }
        en->Release();
    }
    // Microphones with saved custom settings that are not in the list yet.
    HKEY devices = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, (std::wstring(settings::kRegistryPath) + L"\\Devices").c_str(), 0,
                      KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &devices) == ERROR_SUCCESS)
    {
        wchar_t sub[128];
        for (DWORD i = 0;; ++i)
        {
            DWORD len = 128;
            if (RegEnumKeyExW(devices, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            bool known = false;
            for (const Mic& m : mics) known |= _wcsicmp(m.guid.c_str(), sub) == 0;
            if (known || !Settings::IsDeviceCustom(sub)) continue;
            wchar_t name[128] = L"";
            DWORD size = sizeof(name);
            if (RegGetValueW(devices, sub, L"Name", RRF_RT_REG_SZ, nullptr, name, &size) != ERROR_SUCCESS || !name[0])
                wcscpy_s(name, L"Microphone");  // missing, too long or not text
            mics.push_back({sub, name, false});
        }
        RegCloseKey(devices);
    }
    return mics;
}

std::wstring app::DefaultMicGuid()
{
    std::wstring guid;
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&en))))
    {
        IMMDevice* dev = nullptr;
        LPWSTR id = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev)) && SUCCEEDED(dev->GetId(&id)))
            guid = GuidOf(id);
        if (id) CoTaskMemFree(id);
        if (dev) dev->Release();
        en->Release();
    }
    return guid;
}

// ------------------------------------------------------------------------------------ window

namespace
{
    // AGC presets from the AEC3-Echo-Canceller plugin: headroom, max gain, speed, noise ceiling.
    struct AgcPreset { int headroom, maxGain, speed, noise; };
    constexpr std::array<AgcPreset, 3> kAgcPresets{{{8, 20, 3, -60}, {5, 30, 6, -50}, {3, 50, 10, -45}}};

    // Controls, in keyboard (Tab) order.
    enum W { W_DEVICE, W_CUSTOM, W_AEC, W_AEC_LVL, W_NS, W_NS_LVL, W_AGC, W_AGC_LVL, W_CLEANUP, W_LIMITER, W_GAIN, W_LINK, W_RESET, W_COUNT };

    // Layout in DIPs (96 dpi), client area kW x kH.
    constexpr int kW = 440, kH = 556, kPad = 16;
    constexpr int kCardL = 12, kCardR = kW - 12, kRowL = 26, kRowR = kW - 26, kRowH = 44;
    constexpr int kCardA = 104, kCardB = kCardA + 5 * kRowH + 12 + 12;  // processing card, slider card
    constexpr int kSegL = 192, kSegR = 360;                              // choice next to a switch
    int RowTop(int row) { return kCardA + 6 + row * kRowH; }             // rows of the processing card

    const std::vector<std::wstring> kStrength{L"Gentle", L"Normal", L"Strong"};
    const std::vector<std::wstring> kNsLevels{L"6 dB", L"12 dB", L"18 dB", L"21 dB"};
    const std::vector<std::wstring> kCleanup{L"Rumble <80 Hz", L"Hiss >10 kHz"};
    const std::vector<std::wstring> kGainModes{L"Auto", L"Always", L"Never"};

    HWND g_wnd = nullptr;
    UINT g_showMsg = 0;          // "show your settings window" (sent by a second launch)
    UINT g_dpi = 96;
    std::vector<app::Mic> g_mics;  // [0] = "Default (all microphones)"
    int g_sel = 0;
    bool g_custom = false;
    Settings g_current{};
    std::wstring g_status = L"Changes apply immediately.";
    int g_focus = W_DEVICE, g_chip = 0;  // keyboard focus (+ which cleanup chip)
    bool g_cues = false;                 // show the focus ring (after keyboard use)
    int g_hot = -1;                      // control under the mouse

    int S(int v) { return ui::Px(v, g_dpi); }
    RECT Sr(int l, int t, int r, int b) { return {S(l), S(t), S(r), S(b)}; }

    const wchar_t* SelectedGuid()  // nullptr = defaults
    {
        return (g_sel > 0 && g_sel < static_cast<int>(g_mics.size())) ? g_mics[static_cast<size_t>(g_sel)].guid.c_str() : nullptr;
    }
    // Where edits go: the selected microphone if it has custom settings, otherwise the defaults.
    // A microphone that follows the defaults shows them read-only.
    bool Editable() { return !SelectedGuid() || g_custom; }
    const wchar_t* Scope() { return SelectedGuid() && g_custom ? SelectedGuid() : nullptr; }

    int MatchAgcPreset(const Settings& s)
    {
        for (int i = 0; i < static_cast<int>(kAgcPresets.size()); ++i)
        {
            const auto& p = kAgcPresets[static_cast<size_t>(i)];
            if (s.agcHeadroomDb == p.headroom && s.agcMaxGainDb == p.maxGain && s.agcSpeedDbPerSec == p.speed &&
                s.agcNoiseCeilingDbfs == p.noise)
                return i;
        }
        return -1;  // values set by a script: no preset highlighted
    }

    // ---- control geometry (pixels) ----
    RECT RowRect(int row) { return Sr(kRowL, RowTop(row), kRowR, RowTop(row) + kRowH); }
    RECT SegRect(int row, int left = kSegL, int right = kSegR) { return Sr(left, RowTop(row) + 8, right, RowTop(row) + kRowH - 8); }
    RECT GainRow() { return Sr(kRowL, kCardB + 6, kRowR, kCardB + 6 + kRowH); }

    RECT ControlRect(int w)  // what the focus ring surrounds and the mouse hits
    {
        switch (w)
        {
        case W_DEVICE: return Sr(kPad, 34, 290, 70);
        case W_CUSTOM: return ui::SwitchRect(Sr(300, 34, kW - kPad, 70), g_dpi);
        case W_AEC: return ui::SwitchRect(RowRect(0), g_dpi);
        case W_AEC_LVL: return SegRect(0);
        case W_NS: return ui::SwitchRect(RowRect(1), g_dpi);
        case W_NS_LVL: return SegRect(1);
        case W_AGC: return ui::SwitchRect(RowRect(2), g_dpi);
        case W_AGC_LVL: return SegRect(2);
        case W_CLEANUP: return SegRect(3, kSegL, kRowR);
        case W_LIMITER: return ui::SwitchRect(RowRect(4), g_dpi);
        case W_GAIN: { RECT r = GainRow(); return {S(kSegL), r.top + S(8), r.right, r.bottom - S(8)}; }
        case W_LINK: return Sr(kPad, kH - 74, 200, kH - 42);
        case W_RESET: return Sr(kW - kPad - 150, kH - 74, kW - kPad, kH - 42);
        }
        return {};
    }
    // Clicking a row's name also flips its switch.
    bool HitToggleRow(int w, POINT p)
    {
        const int row = w == W_AEC ? 0 : w == W_NS ? 1 : w == W_AGC ? 2 : w == W_LIMITER ? 4 : -1;
        if (row < 0) return false;
        RECT r = RowRect(row);
        RECT name{r.left, r.top, w == W_LIMITER ? ControlRect(w).left : S(kSegL - 8), r.bottom};
        RECT sw = ControlRect(w);
        InflateRect(&sw, S(6), S(10));
        return PtInRect(&name, p) || PtInRect(&sw, p);
    }

    bool Enabled(int w)
    {
        const Settings& s = g_current;
        switch (w)
        {
        case W_DEVICE: case W_LINK: return true;
        case W_CUSTOM: return SelectedGuid() != nullptr;
        case W_AEC: case W_NS: case W_AGC: case W_CLEANUP: case W_LIMITER: case W_RESET: return Editable();
        case W_AEC_LVL: return Editable() && s.aecEnabled;
        case W_NS_LVL: return Editable() && s.nsEnabled;
        case W_AGC_LVL: return Editable() && s.agcEnabled;
        case W_GAIN: return Editable() && !s.agcEnabled;
        }
        return false;
    }

    void Redraw() { InvalidateRect(g_wnd, nullptr, FALSE); }

    // ---- data ----
    void Refresh(const std::wstring& keepGuid)
    {
        g_mics = app::FindMics();
        g_mics.insert(g_mics.begin(), {L"", L"Default (all microphones)", true});
        g_sel = 0;
        for (size_t i = 1; i < g_mics.size(); ++i)
            if (!keepGuid.empty() && _wcsicmp(keepGuid.c_str(), g_mics[i].guid.c_str()) == 0) g_sel = static_cast<int>(i);
    }

    void LoadUi()
    {
        g_custom = SelectedGuid() && Settings::IsDeviceCustom(SelectedGuid());
        g_current = Settings::ReadFromRegistry(Scope());
        Redraw();
    }

    void StatusFor(LONG err, const wchar_t* ok)
    {
        if (err == ERROR_SUCCESS) g_status = ok;
        else if (err == ERROR_FILE_NOT_FOUND) g_status = L"Not installed: the settings key is missing (run the install script).";
        else if (err == ERROR_ACCESS_DENIED) g_status = L"Access denied: run the install script once as administrator.";
        else g_status = L"Could not save settings (error " + std::to_wstring(err) + L").";
    }

    // Applies one change on top of what is in the registry now (keeps values this window doesn't show).
    template <class F>
    void Change(F f)
    {
        if (!Editable()) return;
        Settings s = Settings::ReadFromRegistry(Scope());
        f(s);
        const LONG err = Settings::WriteToRegistry(s, Scope());
        StatusFor(err, L"Saved. Changes apply immediately.");
        g_current = Settings::ReadFromRegistry(Scope());
        Redraw();
    }

    void SelectMic(int i)
    {
        if (i < 0 || i >= static_cast<int>(g_mics.size())) return;
        g_sel = i;
        LoadUi();
    }

    void OpenDeviceList()
    {
        Refresh(SelectedGuid() ? SelectedGuid() : L"");  // microphones may have come or gone
        telemetry::Device live[telemetry::kMaxSlots];
        const int n = telemetry::ReadAll(live);
        std::vector<ui::ListItem> items;
        for (size_t i = 0; i < g_mics.size(); ++i)
        {
            const app::Mic& m = g_mics[i];
            bool inUse = false;
            for (int k = 0; k < n; ++k) inUse |= i > 0 && _wcsicmp(live[k].device, m.guid.c_str()) == 0;
            std::wstring detail = i > 0 && Settings::IsDeviceCustom(m.guid.c_str()) ? L"custom" : L"";
            if (i > 0 && !m.connected) detail = detail.empty() ? L"not connected" : detail + L" \x00B7 not connected";
            if (inUse) detail = detail.empty() ? L"in use" : detail + L" \x00B7 in use";
            items.push_back({m.name, detail, inUse});
        }
        RECT r = ControlRect(W_DEVICE);
        MapWindowPoints(g_wnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
        r.right = r.left + S(kW - 2 * kPad);
        ui::ShowList(g_wnd, r, items, g_sel, [](int i) { SelectMic(i); });
    }

    void Activate(int w, int segment = -1)
    {
        if (!Enabled(w)) return;
        switch (w)
        {
        case W_DEVICE: OpenDeviceList(); break;
        case W_CUSTOM:
        {
            const LONG err = Settings::SetDeviceCustom(SelectedGuid(), !g_custom, g_mics[static_cast<size_t>(g_sel)].name.c_str());
            Refresh(SelectedGuid());  // keeps the same microphone selected (by id)
            LoadUi();
            g_status = err == ERROR_SUCCESS ? L"Saved." : L"Could not save (run the install script once as administrator).";
            break;
        }
        case W_AEC: Change([](Settings& s) { s.aecEnabled = !s.aecEnabled; }); break;
        case W_NS: Change([](Settings& s) { s.nsEnabled = !s.nsEnabled; }); break;
        case W_AGC: Change([](Settings& s) { s.agcEnabled = !s.agcEnabled; }); break;
        case W_LIMITER: Change([](Settings& s) { s.limiterEnabled = !s.limiterEnabled; }); break;
        case W_AEC_LVL: if (segment >= 0) Change([=](Settings& s) { s.aecStrength = static_cast<uint32_t>(segment); }); break;
        case W_NS_LVL: if (segment >= 0) Change([=](Settings& s) { s.nsLevel = static_cast<uint32_t>(segment); }); break;
        case W_GAIN: if (segment >= 0) Change([=](Settings& s) { s.gainMode = static_cast<uint32_t>(segment); }); break;
        case W_AGC_LVL:
            if (segment >= 0)
                Change([=](Settings& s) {
                    const auto& p = kAgcPresets[static_cast<size_t>(segment)];
                    s.agcHeadroomDb = p.headroom; s.agcMaxGainDb = p.maxGain; s.agcSpeedDbPerSec = p.speed; s.agcNoiseCeilingDbfs = p.noise;
                });
            break;
        case W_CLEANUP:
            if (segment == 0) Change([](Settings& s) { s.hpfEnabled = s.hpfEnabled ? 0u : 1u; });
            if (segment == 1) Change([](Settings& s) { s.lpfEnabled = s.lpfEnabled ? 0u : 1u; });
            break;
        case W_LINK: ShellExecuteW(g_wnd, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL); break;
        case W_RESET:
        {
            Settings d{};
            d.bypass = 0;  // defaults also clear a scripted A/B bypass
            const LONG err = Settings::WriteToRegistry(d, Scope());
            LoadUi();
            StatusFor(err, L"Defaults restored.");
            break;
        }
        }
        Redraw();
    }

    // Current choice of a segmented control (for drawing and arrow keys).
    int SegValue(int w)
    {
        const Settings& s = g_current;
        switch (w)
        {
        case W_AEC_LVL: return static_cast<int>(s.aecStrength);
        case W_NS_LVL: return static_cast<int>(s.nsLevel);
        case W_AGC_LVL: return MatchAgcPreset(s);
        case W_GAIN: return static_cast<int>(s.gainMode);
        }
        return -1;
    }
    int SegCount(int w) { return w == W_NS_LVL ? 4 : w == W_CLEANUP ? 2 : 3; }

    // ---- painting ----
    void Paint()
    {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(g_wnd, &ps);
        RECT client;
        GetClientRect(g_wnd, &client);
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bmp = CreateCompatibleBitmap(wdc, client.right, client.bottom);
        auto oldBmp = SelectObject(dc, bmp);
        const ui::Theme t = ui::CurrentTheme();
        const Settings& s = g_current;
        const bool edit = Editable();
        HBRUSH bg = CreateSolidBrush(t.bg);
        FillRect(dc, &client, bg);
        DeleteObject(bg);
        HFONT body = ui::Font(10, FW_NORMAL, g_dpi), smallFont = ui::Font(9, FW_NORMAL, g_dpi), icons = ui::IconFont(10, g_dpi);
        auto oldFont = SelectObject(dc, smallFont);

        // Header: microphone + custom switch + what the switches change.
        ui::Text(dc, L"Microphone", Sr(kPad, 12, 290, 32), t.subtle);
        SelectObject(dc, body);
        ui::DrawDropdown(dc, ControlRect(W_DEVICE), g_mics.empty() ? L"" : g_mics[static_cast<size_t>(g_sel)].name, true,
                         g_hot == W_DEVICE || ui::ListOpen(), body, icons, t, g_dpi);
        if (SelectedGuid())
        {
            RECT cr = Sr(300, 34, kW - kPad, 70);
            cr.right = ControlRect(W_CUSTOM).left - S(8);
            ui::Text(dc, L"Custom", cr, t.text, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            ui::DrawSwitch(dc, ControlRect(W_CUSTOM), g_custom, true, t, g_dpi);
        }
        SelectObject(dc, smallFont);
        ui::Text(dc, !SelectedGuid() ? L"These settings apply to every microphone without custom settings."
                     : g_custom      ? L"This microphone uses its own settings."
                                     : L"Uses the default settings. Switch on Custom to give it its own.",
                 Sr(kPad, 74, kW - kPad, 96), t.subtle);

        // Processing card.
        ui::FillRound(dc, Sr(kCardL, kCardA, kCardR, kCardA + 5 * kRowH + 12), S(8), t.card);
        auto rowName = [&](int row, const wchar_t* name, int right = kSegL - 8) {
            SelectObject(dc, body);
            RECT r = RowRect(row);
            r.right = S(right);
            ui::Text(dc, name, r, edit ? t.text : t.disabled);
            if (row > 0)
            {
                RECT line{RowRect(row).left, RowRect(row).top, RowRect(row).right, RowRect(row).top + 1};
                HBRUSH b = CreateSolidBrush(t.line);
                FillRect(dc, &line, b);
                DeleteObject(b);
            }
        };
        rowName(0, L"Echo cancellation");
        ui::DrawSegmented(dc, ControlRect(W_AEC_LVL), kStrength, SegValue(W_AEC_LVL), Enabled(W_AEC_LVL), false, smallFont, t, g_dpi);
        ui::DrawSwitch(dc, ControlRect(W_AEC), s.aecEnabled, edit, t, g_dpi);
        rowName(1, L"Noise suppression");
        ui::DrawSegmented(dc, ControlRect(W_NS_LVL), kNsLevels, SegValue(W_NS_LVL), Enabled(W_NS_LVL), false, smallFont, t, g_dpi);
        ui::DrawSwitch(dc, ControlRect(W_NS), s.nsEnabled, edit, t, g_dpi);
        rowName(2, L"Automatic gain");
        ui::DrawSegmented(dc, ControlRect(W_AGC_LVL), kStrength, SegValue(W_AGC_LVL), Enabled(W_AGC_LVL), false, smallFont, t, g_dpi);
        ui::DrawSwitch(dc, ControlRect(W_AGC), s.agcEnabled, edit, t, g_dpi);
        rowName(3, L"Cleanup filters");
        ui::DrawSegmented(dc, ControlRect(W_CLEANUP), kCleanup, (s.hpfEnabled ? 1 : 0) | (s.lpfEnabled ? 2 : 0), edit, true, smallFont, t, g_dpi);
        rowName(4, L"Limiter", 120);
        {
            SelectObject(dc, smallFont);
            RECT d = RowRect(4);
            d.left = S(120);
            d.right = ControlRect(W_LIMITER).left - S(12);
            ui::Text(dc, L"keeps peaks below \x2212" L"1 dBFS", d, edit ? t.subtle : t.disabled, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        ui::DrawSwitch(dc, ControlRect(W_LIMITER), s.limiterEnabled, edit, t, g_dpi);

        // Microphone volume slider card.
        ui::FillRound(dc, Sr(kCardL, kCardB, kCardR, kCardB + kRowH + 64), S(8), t.card);
        SelectObject(dc, body);
        {
            RECT r = GainRow();
            r.right = S(kSegL - 8);
            ui::Text(dc, L"Mic volume slider", r, Enabled(W_GAIN) ? t.text : t.disabled);
        }
        ui::DrawSegmented(dc, ControlRect(W_GAIN), kGainModes, SegValue(W_GAIN), Enabled(W_GAIN), false, smallFont, t, g_dpi);
        SelectObject(dc, smallFont);
        // USB / Bluetooth microphones apply the slider in their own hardware (e.g. a webcam's preamp), before
        // any effect: AGC cannot switch that off, only even out what arrives.
        ui::Text(dc,
                 s.agcEnabled ? L"Automatic gain sets the level, so the slider is not applied here. USB and Bluetooth microphones "
                                L"still apply it in their own hardware: keep it moderate, so the microphone does not clip."
                              : L"Auto applies the slider in software for built-in microphones. USB and Bluetooth microphones "
                                L"apply it in their own hardware.",
                 Sr(kRowL, kCardB + 6 + kRowH, kRowR, kCardB + kRowH + 60), t.subtle, DT_LEFT | DT_TOP | DT_WORDBREAK);

        // Footer.
        ui::Text(dc, L"Turn it on per microphone in Sound settings > Audio enhancements.",
                 Sr(kPad, kH - 98, kW - kPad, kH - 78), t.subtle);
        SelectObject(dc, body);
        ui::Text(dc, L"Open sound settings", ControlRect(W_LINK), t.accent);
        {
            RECT b = ControlRect(W_RESET);
            ui::FillRound(dc, b, S(6), !edit ? t.control : g_hot == W_RESET ? t.controlHover : t.control);
            ui::Text(dc, L"Reset to defaults", b, edit ? t.text : t.disabled, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(dc, smallFont);
        ui::Text(dc, g_status, Sr(kPad, kH - 34, kW - kPad, kH - 12), t.subtle);

        // Keyboard focus ring.
        if (g_cues && GetFocus() == g_wnd && !ui::ListOpen())
        {
            RECT f = ControlRect(g_focus);
            if (g_focus == W_CLEANUP)
            {
                const int w = f.right - f.left;
                f = {f.left + w * g_chip / 2, f.top, f.left + w * (g_chip + 1) / 2, f.bottom};
            }
            const bool pill = g_focus == W_CUSTOM || g_focus == W_AEC || g_focus == W_NS || g_focus == W_AGC || g_focus == W_LIMITER;
            ui::DrawFocus(dc, f, pill ? (f.bottom - f.top) / 2 : S(6), t, g_dpi);
        }

        SelectObject(dc, oldFont);
        DeleteObject(body);
        DeleteObject(smallFont);
        DeleteObject(icons);
        BitBlt(wdc, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(g_wnd, &ps);
    }

    // ---- input ----
    int HitTest(POINT p, int* segment)
    {
        *segment = -1;
        for (int w : {W_DEVICE, W_AEC_LVL, W_NS_LVL, W_AGC_LVL, W_CLEANUP, W_GAIN, W_LINK, W_RESET})
        {
            const RECT r = ControlRect(w);
            if (PtInRect(&r, p))
            {
                *segment = ui::SegmentAt(r, SegCount(w), p);
                return w;
            }
        }
        for (int w : {W_AEC, W_NS, W_AGC, W_LIMITER})
            if (HitToggleRow(w, p)) return w;
        RECT custom = Sr(300, 34, kW - kPad, 70);
        if (SelectedGuid() && PtInRect(&custom, p)) return W_CUSTOM;
        return -1;
    }

    void MoveFocus(int dir)
    {
        for (int k = 0, w = g_focus; k < W_COUNT; ++k)
        {
            w = (w + dir + W_COUNT) % W_COUNT;
            if (Enabled(w)) { g_focus = w; g_chip = 0; break; }
        }
        g_cues = true;
        Redraw();
    }

    void Key(WPARAM vk)
    {
        if (ui::ListOpen()) { ui::ListKey(vk); return; }
        if (!Enabled(g_focus)) MoveFocus(+1);
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        const bool seg = g_focus == W_AEC_LVL || g_focus == W_NS_LVL || g_focus == W_AGC_LVL || g_focus == W_GAIN;
        switch (vk)
        {
        case VK_TAB: MoveFocus(shift ? -1 : +1); return;
        case VK_SPACE:
        case VK_RETURN:
            g_cues = true;
            if (g_focus == W_CLEANUP) Activate(W_CLEANUP, g_chip);
            else if (!seg) Activate(g_focus);
            return;
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        {
            g_cues = true;
            const int d = (vk == VK_LEFT || vk == VK_UP) ? -1 : +1;
            if (seg)
            {
                const int v = SegValue(g_focus);
                const int next = v < 0 ? (d > 0 ? 0 : SegCount(g_focus) - 1) : v + d;
                if (next >= 0 && next < SegCount(g_focus)) Activate(g_focus, next);
            }
            else if (g_focus == W_CLEANUP) { g_chip = d > 0 ? 1 : 0; Redraw(); }
            else if (g_focus == W_DEVICE && (vk == VK_UP || vk == VK_DOWN))
            {
                if (GetKeyState(VK_MENU) < 0) OpenDeviceList();
                else { Refresh(SelectedGuid() ? SelectedGuid() : L""); SelectMic(std::clamp(g_sel + d, 0, static_cast<int>(g_mics.size()) - 1)); }
            }
            return;
        }
        case VK_F4:
            if (g_focus == W_DEVICE) OpenDeviceList();
            return;
        }
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
        {
            g_cues = false;
            int segment = -1;
            const int w = HitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, &segment);
            if (w >= 0 && Enabled(w))
            {
                g_focus = w;
                if (w == W_CLEANUP && segment >= 0) g_chip = segment;
                Activate(w, segment);
            }
            Redraw();
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            int segment = -1;
            int w = HitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, &segment);
            if (w >= 0 && !Enabled(w)) w = -1;
            if (w != g_hot)
            {
                g_hot = w;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                Redraw();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_hot = -1;
            Redraw();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT)
            {
                SetCursor(LoadCursorW(nullptr, g_hot >= 0 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp == VK_TAB || wp == VK_SPACE || wp == VK_RETURN || wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP ||
                wp == VK_DOWN || wp == VK_F4 || wp == VK_ESCAPE || wp == VK_HOME || wp == VK_END)
            {
                if (wp == VK_ESCAPE && !ui::ListOpen()) { ShowWindow(hwnd, SW_HIDE); return 0; }
                Key(wp);
                return 0;
            }
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            Redraw();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) ui::CloseList();
            break;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            ui::ApplyWindowTheme(hwnd, ui::CurrentTheme(), false);
            Redraw();
            return 0;
        case WM_DPICHANGED:
        {
            g_dpi = HIWORD(wp);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Redraw();
            return 0;
        }
        case WM_CLOSE:
            ui::CloseList();
            ShowWindow(hwnd, SW_HIDE);  // keep running in the tray
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        if (msg == g_showMsg && g_showMsg)
        {
            app::ShowMainWindow();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void app::ShowMainWindow(const wchar_t* micGuid)
{
    Refresh(micGuid ? micGuid : SelectedGuid() ? SelectedGuid() : L"");
    LoadUi();
    ShowWindow(g_wnd, SW_RESTORE);
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
}

void app::ExitApp()
{
    ui::CloseList();
    tray::Shutdown();
    DestroyWindow(g_wnd);  // WM_DESTROY -> PostQuitMessage
}

// "WebrtcAudioEngineSettings.exe"        : tray + open the settings window
// "WebrtcAudioEngineSettings.exe /tray"  : tray only (started at sign-in)
// "... /install [version]", "... /uninstall": set up / remove the app (Setup.cpp, no window)
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int show)
{
    if (cmdLine && wcsstr(cmdLine, L"/install")) return setup::Install(cmdLine);
    if (cmdLine && wcsstr(cmdLine, L"/uninstall")) return setup::Uninstall();
    const bool trayOnly = cmdLine && wcsstr(cmdLine, L"/tray") != nullptr;
    g_showMsg = RegisterWindowMessageW(L"WebrtcAudioEngine.ShowSettings");

    // Single instance: a second launch asks the running one to show its window.
    // ERROR_ACCESS_DENIED: the running copy is elevated, so its mutex is not ours to open, but it exists.
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"WebrtcAudioEngineSettings.SingleInstance");
    const DWORD mutexErr = GetLastError();
    if (mutexErr == ERROR_ALREADY_EXISTS || (!instanceMutex && mutexErr == ERROR_ACCESS_DENIED))
    {
        if (!trayOnly)
            if (HWND other = FindWindowW(app::kWindowClass, nullptr))
            {
                AllowSetForegroundWindow(ASFW_ANY);
                PostMessageW(other, g_showMsg, 0, 0);
            }
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // device enumeration (MMDevice API)

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = app::kWindowClass;
    g_dpi = GetDpiForSystem();
    // App icon from the resources (SettingsApp.rc); the tray icon is drawn at runtime in TrayFlyout.cpp.
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXICON, g_dpi),
                                             GetSystemMetricsForDpi(SM_CYICON, g_dpi), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXSMICON, g_dpi),
                                               GetSystemMetricsForDpi(SM_CYSMICON, g_dpi), LR_DEFAULTCOLOR));
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, S(kW), S(kH)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, g_dpi);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"WebRTC Audio Engine Settings", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    g_dpi = GetDpiForWindow(g_wnd);  // the monitor it opened on
    rc = {0, 0, S(kW), S(kH)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, g_dpi);
    SetWindowPos(g_wnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ui::ApplyWindowTheme(g_wnd, ui::CurrentTheme(), false);
    ChangeWindowMessageFilterEx(g_wnd, g_showMsg, MSGFLT_ALLOW, nullptr);  // a non-elevated second launch may ask us to show
    Refresh(app::DefaultMicGuid());
    LoadUi();
    tray::Init(inst);
    if (!trayOnly)
        ShowWindow(g_wnd, show);

    // Refresh when the settings change elsewhere (scripts, another tool), including the per-microphone
    // subkeys. If the key does not exist yet, look again every few seconds.
    // The installer replacing or removing the app bumps AppGeneration in the app key: then exit.
    const DWORD generation = app::AppGeneration();
    HKEY appKey = nullptr;
    HANDLE appChanged = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto watchApp = [&] {
        if (!appKey && RegOpenKeyExW(HKEY_LOCAL_MACHINE, app::kAppKey, 0, KEY_NOTIFY | KEY_QUERY_VALUE, &appKey) != ERROR_SUCCESS)
        {
            appKey = nullptr;
            return;
        }
        RegNotifyChangeKeyValue(appKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, appChanged, TRUE);
    };
    watchApp();
    HKEY key = nullptr;
    HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto watch = [&] {
        if (!key && RegOpenKeyExW(HKEY_LOCAL_MACHINE, settings::kRegistryPath, 0, KEY_NOTIFY | KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        {
            key = nullptr;
            return false;
        }
        RegNotifyChangeKeyValue(key, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME, changed, TRUE);
        return true;
    };
    if (!watch())
        g_status = L"Not installed: the settings key is missing (run the install script).";

    MSG m;
    for (;;)
    {
        const HANDLE events[] = {changed, appChanged};
        const DWORD r = MsgWaitForMultipleObjects(2, events, FALSE, key && appKey ? INFINITE : 5000, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0 + 1 || (r == WAIT_TIMEOUT && !appKey))
        {
            watchApp();
            if (appKey && app::AppGeneration() != generation)
                app::ExitApp();  // WM_QUIT follows
            if (r == WAIT_OBJECT_0 + 1) continue;
        }
        if (r == WAIT_OBJECT_0 || (r == WAIT_TIMEOUT && !key && watch()))
        {
            if (r == WAIT_OBJECT_0) watch();  // re-arm before reading: values written meanwhile notify again
            const bool custom = SelectedGuid() && Settings::IsDeviceCustom(SelectedGuid());
            if (custom != g_custom || !(Settings::ReadFromRegistry(Scope()) == g_current))
                LoadUi();
            tray::SettingsChanged();
            if (r != WAIT_OBJECT_0) { g_status = L"Changes apply immediately."; Redraw(); }  // the settings key showed up
            continue;
        }
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
        {
            if (m.message == WM_QUIT)
                return static_cast<int>(m.wParam);
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}
