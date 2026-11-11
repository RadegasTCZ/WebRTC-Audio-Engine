// SPDX-License-Identifier: GPL-3.0-or-later
// WebRTC Audio Engine Settings: a small native window over HKLM\SOFTWARE\WebrtcVoiceApo.
// Every change is written immediately; the audio effect picks it up live (within ~10 ms).
// The window also refreshes itself when the settings change elsewhere (scripts, another instance).
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <array>
#include <string>
#include "../src/Settings.h"

namespace
{
    enum : int
    {
        IDC_ENABLE = 100, IDC_AEC_ON, IDC_AEC_STRENGTH, IDC_NS_ON, IDC_NS_LEVEL, IDC_AGC_ON, IDC_AGC_PRESET,
        IDC_CLEANUP, IDC_LIM_ON, IDC_GAIN_MODE, IDC_DEFAULTS, IDC_SOUND_SETTINGS, IDC_STATUS, IDC_GAIN_NOTE
    };

    // AGC presets from the AEC3-Echo-Canceller plugin: headroom, max gain, speed, noise ceiling.
    struct AgcPreset { const wchar_t* name; int headroom, maxGain, speed, noise; };
    constexpr std::array<AgcPreset, 3> kAgcPresets{{
        {L"Gentle", 8, 20, 3, -60}, {L"Normal", 5, 30, 6, -50}, {L"Strong", 3, 50, 10, -45}}};

    HWND g_wnd = nullptr;
    HFONT g_font = nullptr, g_bold = nullptr;
    UINT g_dpi = 96;
    bool g_updating = false;   // true while the UI is being filled from the registry
    Settings g_current{};

    int S(int v) { return MulDiv(v, static_cast<int>(g_dpi), 96); }

    HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, bool bold = false)
    {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(bold ? g_bold : g_font), TRUE);
        return c;
    }
    HWND Item(int id) { return GetDlgItem(g_wnd, id); }
    void Combo(int id, std::initializer_list<const wchar_t*> items)
    {
        for (auto* s : items) SendMessageW(Item(id), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    }
    int Sel(int id) { return static_cast<int>(SendMessageW(Item(id), CB_GETCURSEL, 0, 0)); }
    void SetSel(int id, int i) { SendMessageW(Item(id), CB_SETCURSEL, i, 0); }
    bool Checked(int id) { return SendMessageW(Item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    void SetChecked(int id, bool on) { SendMessageW(Item(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
    void Status(const std::wstring& text) { SetWindowTextW(Item(IDC_STATUS), text.c_str()); }

    int MatchAgcPreset(const Settings& s)
    {
        for (int i = 0; i < static_cast<int>(kAgcPresets.size()); ++i)
        {
            const auto& p = kAgcPresets[i];
            if (s.agcHeadroomDb == p.headroom && s.agcMaxGainDb == p.maxGain && s.agcSpeedDbPerSec == p.speed &&
                s.agcNoiseCeilingDbfs == p.noise)
                return i;
        }
        return 3;  // Custom
    }

    void UpdateEnabledStates()
    {
        const bool on = Checked(IDC_ENABLE);
        for (int id : {IDC_AEC_ON, IDC_NS_ON, IDC_AGC_ON, IDC_CLEANUP, IDC_LIM_ON, IDC_GAIN_MODE})
            EnableWindow(Item(id), on);
        EnableWindow(Item(IDC_AEC_STRENGTH), on && Checked(IDC_AEC_ON));
        EnableWindow(Item(IDC_NS_LEVEL), on && Checked(IDC_NS_ON));
        EnableWindow(Item(IDC_AGC_PRESET), on && Checked(IDC_AGC_ON));
        EnableWindow(Item(IDC_GAIN_MODE), on && !Checked(IDC_AGC_ON));
        ShowWindow(Item(IDC_GAIN_NOTE), Checked(IDC_AGC_ON) ? SW_SHOW : SW_HIDE);
    }

    void LoadUi()
    {
        g_updating = true;
        g_current = Settings::ReadFromRegistry();
        const Settings& s = g_current;
        SetChecked(IDC_ENABLE, !s.bypass);
        SetChecked(IDC_AEC_ON, s.aecEnabled);
        SetSel(IDC_AEC_STRENGTH, static_cast<int>(s.aecStrength));
        SetChecked(IDC_NS_ON, s.nsEnabled);
        SetSel(IDC_NS_LEVEL, static_cast<int>(s.nsLevel));
        SetChecked(IDC_AGC_ON, s.agcEnabled);
        // "Custom" is only listed while the values match no preset (set by scripts).
        HWND preset = Item(IDC_AGC_PRESET);
        const int match = MatchAgcPreset(s);
        if (SendMessageW(preset, CB_GETCOUNT, 0, 0) == 4) SendMessageW(preset, CB_DELETESTRING, 3, 0);
        if (match == 3) SendMessageW(preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Custom"));
        SetSel(IDC_AGC_PRESET, match);
        SetSel(IDC_CLEANUP, static_cast<int>((s.hpfEnabled ? 1u : 0u) | (s.lpfEnabled ? 2u : 0u)));
        SetChecked(IDC_LIM_ON, s.limiterEnabled);
        SetSel(IDC_GAIN_MODE, static_cast<int>(s.gainMode));
        UpdateEnabledStates();
        g_updating = false;
    }

    void SaveFromUi(int changedId)
    {
        if (g_updating)
            return;
        Settings s = Settings::ReadFromRegistry();  // keep values the window does not show (frequencies, AGC details)
        s.bypass = Checked(IDC_ENABLE) ? 0u : 1u;
        s.aecEnabled = Checked(IDC_AEC_ON);
        s.aecStrength = static_cast<uint32_t>(Sel(IDC_AEC_STRENGTH));
        s.nsEnabled = Checked(IDC_NS_ON);
        s.nsLevel = static_cast<uint32_t>(Sel(IDC_NS_LEVEL));
        s.agcEnabled = Checked(IDC_AGC_ON);
        if (changedId == IDC_AGC_PRESET && Sel(IDC_AGC_PRESET) < 3)
        {
            const auto& p = kAgcPresets[static_cast<size_t>(Sel(IDC_AGC_PRESET))];
            s.agcHeadroomDb = p.headroom; s.agcMaxGainDb = p.maxGain; s.agcSpeedDbPerSec = p.speed; s.agcNoiseCeilingDbfs = p.noise;
        }
        const int cleanup = Sel(IDC_CLEANUP);
        s.hpfEnabled = (cleanup & 1) ? 1u : 0u;
        s.lpfEnabled = (cleanup & 2) ? 1u : 0u;
        s.limiterEnabled = Checked(IDC_LIM_ON);
        s.gainMode = static_cast<uint32_t>(Sel(IDC_GAIN_MODE));
        const LONG err = Settings::WriteToRegistry(s);
        if (err == ERROR_SUCCESS)
            Status(L"Saved. Changes apply immediately.");
        else if (err == ERROR_FILE_NOT_FOUND)
            Status(L"Not installed: the settings key is missing (run the install script).");
        else if (err == ERROR_ACCESS_DENIED)
            Status(L"Access denied: run the install script once as administrator.");
        else
            Status(L"Could not save settings (error " + std::to_wstring(err) + L").");
        UpdateEnabledStates();
    }

    void BuildUi()
    {
        int y = 12;
        Ctl(L"STATIC", L"Choose \"WebRTC Audio Engine\" for your microphone under Sound settings > "
                       L"(microphone) > Audio enhancements.", SS_LEFT, 16, y, 368, 32, 0);
        y += 36;
        Ctl(L"BUTTON", L"Open sound settings", BS_PUSHBUTTON, 16, y, 150, 26, IDC_SOUND_SETTINGS);
        y += 40;
        Ctl(L"BUTTON", L"Voice processing enabled", BS_AUTOCHECKBOX, 16, y, 300, 22, IDC_ENABLE, true);
        y += 34;

        auto section = [&](const wchar_t* title, int height) {
            Ctl(L"BUTTON", title, BS_GROUPBOX, 16, y, 368, height, 0, true);
            const int inner = y + 22;
            y += height + 8;
            return inner;
        };
        int r = section(L"Echo cancellation (AEC3)", 56);
        Ctl(L"BUTTON", L"On", BS_AUTOCHECKBOX, 28, r, 80, 24, IDC_AEC_ON);
        Ctl(L"STATIC", L"Strength", SS_RIGHT, 140, r + 4, 80, 20, 0);
        Ctl(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 228, r, 140, 200, IDC_AEC_STRENGTH);
        Combo(IDC_AEC_STRENGTH, {L"Gentle", L"Normal", L"Strong"});

        r = section(L"Noise suppression", 56);
        Ctl(L"BUTTON", L"On", BS_AUTOCHECKBOX, 28, r, 80, 24, IDC_NS_ON);
        Ctl(L"STATIC", L"Level", SS_RIGHT, 140, r + 4, 80, 20, 0);
        Ctl(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 228, r, 140, 200, IDC_NS_LEVEL);
        Combo(IDC_NS_LEVEL, {L"6 dB", L"12 dB", L"18 dB", L"21 dB"});

        r = section(L"Automatic gain (AGC2)", 56);
        Ctl(L"BUTTON", L"On", BS_AUTOCHECKBOX, 28, r, 80, 24, IDC_AGC_ON);
        Ctl(L"STATIC", L"Preset", SS_RIGHT, 140, r + 4, 80, 20, 0);
        Ctl(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 228, r, 140, 200, IDC_AGC_PRESET);
        Combo(IDC_AGC_PRESET, {L"Gentle", L"Normal", L"Strong"});

        r = section(L"Cleanup filters", 56);
        Ctl(L"STATIC", L"Remove", SS_LEFT, 28, r + 4, 100, 20, 0);
        Ctl(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 140, r, 228, 200, IDC_CLEANUP);
        Combo(IDC_CLEANUP, {L"Off", L"Rumble (below 80 Hz)", L"Hiss (above 10 kHz)", L"Rumble and hiss"});

        r = section(L"Output limiter", 52);
        Ctl(L"BUTTON", L"Keep peaks below -1 dBFS", BS_AUTOCHECKBOX, 28, r, 300, 24, IDC_LIM_ON);

        r = section(L"Microphone volume slider", 76);
        Ctl(L"STATIC", L"Apply", SS_LEFT, 28, r + 4, 100, 20, 0);
        Ctl(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 140, r, 228, 200, IDC_GAIN_MODE);
        Combo(IDC_GAIN_MODE, {L"Auto (built-in microphones)", L"Always", L"Never"});
        Ctl(L"STATIC", L"Ignored while automatic gain is on.", SS_LEFT, 28, r + 30, 340, 20, IDC_GAIN_NOTE);

        Ctl(L"BUTTON", L"Reset to defaults", BS_PUSHBUTTON, 16, y + 4, 150, 26, IDC_DEFAULTS);
        y += 40;
        Ctl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 16, y, 368, 20, IDC_STATUS);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_COMMAND:
        {
            const int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDC_SOUND_SETTINGS)
                ShellExecuteW(hwnd, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
            else if (id == IDC_DEFAULTS)
            {
                const LONG err = Settings::WriteToRegistry(Settings{});
                LoadUi();
                Status(err == ERROR_SUCCESS ? L"Defaults restored." : L"Could not save settings.");
            }
            else if ((code == BN_CLICKED && id >= IDC_ENABLE) || code == CBN_SELCHANGE)
                SaveFromUi(id);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    // Single instance: bring the existing window to the front.
    CreateMutexW(nullptr, TRUE, L"WebrtcAudioEngineSettings.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (HWND other = FindWindowW(L"WebrtcAudioEngineSettings", nullptr))
        {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"WebrtcAudioEngineSettings";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_dpi = GetDpiForSystem();
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_dpi);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    g_bold = CreateFontIndirectW(&bold);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, S(400), S(640)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, g_dpi);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"WebRTC Audio Engine Settings", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    BuildUi();
    LoadUi();
    Status(L"Changes apply immediately.");
    ShowWindow(g_wnd, show);

    // Refresh when the settings change elsewhere (scripts, another tool).
    HKEY key = nullptr;
    HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, settings::kRegistryPath, 0, KEY_NOTIFY | KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
        RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, changed, TRUE);
    else
        Status(L"Not installed: the settings key is missing (run the install script).");

    MSG m;
    for (;;)
    {
        const DWORD r = MsgWaitForMultipleObjects(1, &changed, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0)
        {
            if (!(Settings::ReadFromRegistry() == g_current))
                LoadUi();
            if (key) RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, changed, TRUE);
            continue;
        }
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
        {
            if (m.message == WM_QUIT)
                return static_cast<int>(m.wParam);
            if (!IsDialogMessageW(g_wnd, &m))
            {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
    }
}
