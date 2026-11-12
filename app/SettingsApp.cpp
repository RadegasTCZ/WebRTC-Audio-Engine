// SPDX-License-Identifier: GPL-3.0-or-later
// WebRTC Audio Engine Settings: a small native window over HKLM\SOFTWARE\WebrtcVoiceApo.
// Every change is written immediately; the audio effect picks it up live (within ~10 ms).
// The window also refreshes itself when the settings change elsewhere (scripts, another instance).
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <array>
#include <string>
#include <vector>
#include "../src/Settings.h"
#include "App.h"

namespace
{
    enum : int
    {
        IDC_FIRST = 100, IDC_AEC_ON, IDC_AEC_STRENGTH, IDC_NS_ON, IDC_NS_LEVEL, IDC_AGC_ON, IDC_AGC_PRESET,
        IDC_CLEANUP, IDC_LIM_ON, IDC_GAIN_MODE, IDC_DEFAULTS, IDC_SOUND_SETTINGS, IDC_STATUS, IDC_GAIN_NOTE,
        IDC_DEVICE, IDC_CUSTOM, IDC_SCOPE_NOTE
    };
    // Controls that edit settings (disabled while a microphone follows the defaults).
    constexpr int kSettingControls[] = {IDC_AEC_ON, IDC_AEC_STRENGTH, IDC_NS_ON, IDC_NS_LEVEL, IDC_AGC_ON, IDC_AGC_PRESET,
                                        IDC_CLEANUP, IDC_LIM_ON, IDC_GAIN_MODE};

    // ---------------------------------------------------------------- microphones
    // "Default (all microphones)" + every active microphone that has WebRTC Audio Engine selected,
    // + microphones that have custom settings saved (even if not in use right now).
    struct Mic { std::wstring guid, name; };
    std::vector<Mic> g_mics;   // [0] = defaults (empty guid)
    constexpr wchar_t kOurPack[] = L"{9879C461-0BA4-4A7B-9F35-3C04EAC1772D}";
    constexpr PROPERTYKEY kSelectedPack = {{0xc876062a, 0xa276, 0x4ed6, {0xb1, 0x5b, 0x39, 0x62, 0xe6, 0x90, 0x07, 0xf8}}, 2};

    void FindMics()
    {
        g_mics.clear();
        g_mics.push_back({L"", L"Default (all microphones)"});
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
                        const bool ours = pack.vt == VT_LPWSTR && _wcsicmp(pack.pwszVal, kOurPack) == 0;
                        if (ours)
                        {
                            const wchar_t* brace = wcsrchr(id, L'{');
                            g_mics.push_back({brace ? brace : id, name.vt == VT_LPWSTR ? name.pwszVal : L"Microphone"});
                        }
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
                for (const Mic& m : g_mics) known |= _wcsicmp(m.guid.c_str(), sub) == 0;
                if (known || !Settings::IsDeviceCustom(sub)) continue;
                wchar_t name[128] = L"";
                DWORD size = sizeof(name);
                if (RegGetValueW(devices, sub, L"Name", RRF_RT_REG_SZ, nullptr, name, &size) != ERROR_SUCCESS || !name[0])
                    wcscpy_s(name, L"Microphone (not in use)");  // missing, too long or not text
                g_mics.push_back({sub, name});
            }
            RegCloseKey(devices);
        }
    }

    const wchar_t* SelectedGuid();  // nullptr = defaults

    // AGC presets from the AEC3-Echo-Canceller plugin: headroom, max gain, speed, noise ceiling.
    struct AgcPreset { const wchar_t* name; int headroom, maxGain, speed, noise; };
    constexpr std::array<AgcPreset, 3> kAgcPresets{{
        {L"Gentle", 8, 20, 3, -60}, {L"Normal", 5, 30, 6, -50}, {L"Strong", 3, 50, 10, -45}}};

    HWND g_wnd = nullptr;
    UINT g_showMsg = 0;          // "show your settings window" (sent by a second launch)
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

    const wchar_t* SelectedGuid()
    {
        const int i = Sel(IDC_DEVICE);
        return (i > 0 && i < static_cast<int>(g_mics.size())) ? g_mics[static_cast<size_t>(i)].guid.c_str() : nullptr;
    }
    // Where edits go: the selected microphone if it has custom settings, otherwise the defaults.
    // A microphone that follows the defaults shows them read-only.
    bool Editable() { return !SelectedGuid() || Checked(IDC_CUSTOM); }
    const wchar_t* Scope() { return SelectedGuid() && Checked(IDC_CUSTOM) ? SelectedGuid() : nullptr; }

    void PopulateDeviceCombo()
    {
        const std::wstring keep = SelectedGuid() ? SelectedGuid() : L"";
        FindMics();
        HWND combo = Item(IDC_DEVICE);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        int sel = 0;
        for (size_t i = 0; i < g_mics.size(); ++i)
        {
            std::wstring label = g_mics[i].name;
            if (i > 0 && Settings::IsDeviceCustom(g_mics[i].guid.c_str())) label += L"  (custom)";
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (!keep.empty() && _wcsicmp(keep.c_str(), g_mics[i].guid.c_str()) == 0) sel = static_cast<int>(i);
        }
        SetSel(IDC_DEVICE, sel);
    }

    // Turning everything off is Windows' own job (Audio enhancements: Off), so there is no master switch.
    void UpdateEnabledStates()
    {
        const bool edit = Editable();
        for (int id : kSettingControls) EnableWindow(Item(id), edit);
        EnableWindow(Item(IDC_AEC_STRENGTH), edit && Checked(IDC_AEC_ON));
        EnableWindow(Item(IDC_NS_LEVEL), edit && Checked(IDC_NS_ON));
        EnableWindow(Item(IDC_AGC_PRESET), edit && Checked(IDC_AGC_ON));
        EnableWindow(Item(IDC_GAIN_MODE), edit && !Checked(IDC_AGC_ON));
        EnableWindow(Item(IDC_DEFAULTS), edit);
        const bool mic = SelectedGuid() != nullptr;
        ShowWindow(Item(IDC_CUSTOM), mic ? SW_SHOW : SW_HIDE);
        SetWindowTextW(Item(IDC_SCOPE_NOTE), !mic                   ? L"These settings apply to every microphone without custom settings."
                                             : Checked(IDC_CUSTOM) ? L"This microphone uses its own settings."
                                                                   : L"This microphone follows the default settings.");
        ShowWindow(Item(IDC_GAIN_NOTE), Checked(IDC_AGC_ON) ? SW_SHOW : SW_HIDE);
    }

    void LoadUi()
    {
        g_updating = true;
        SetChecked(IDC_CUSTOM, SelectedGuid() && Settings::IsDeviceCustom(SelectedGuid()));
        g_current = Settings::ReadFromRegistry(Scope());
        const Settings& s = g_current;
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
        if (g_updating || !Editable())
            return;
        Settings s = Settings::ReadFromRegistry(Scope());  // keep values the window does not show (frequencies, AGC details)
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
        const LONG err = Settings::WriteToRegistry(s, Scope());
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
        Ctl(L"STATIC", L"Choose \"WebRTC Audio Engine\" under Sound settings > (microphone) > "
                       L"Audio enhancements. Choosing \"Off\" there turns all processing off.", SS_LEFT, 16, y, 368, 32, 0);
        y += 36;
        Ctl(L"BUTTON", L"Open sound settings", BS_PUSHBUTTON, 16, y, 150, 26, IDC_SOUND_SETTINGS);
        y += 40;
        Ctl(L"STATIC", L"Microphone", SS_LEFT, 16, y + 4, 90, 20, 0, true);
        Ctl(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 110, y, 274, 240, IDC_DEVICE);
        y += 32;
        Ctl(L"BUTTON", L"Custom settings for this microphone", BS_AUTOCHECKBOX, 16, y, 368, 22, IDC_CUSTOM);
        y += 24;
        Ctl(L"STATIC", L"", SS_LEFT, 16, y, 368, 20, IDC_SCOPE_NOTE);
        y += 28;

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
            else if (id == IDC_DEVICE && code == CBN_DROPDOWN)
                PopulateDeviceCombo();  // microphones may have started/stopped using the effect pack
            else if (id == IDC_DEVICE && code == CBN_SELCHANGE)
                LoadUi();
            else if (id == IDC_CUSTOM && code == BN_CLICKED)
            {
                const int i = Sel(IDC_DEVICE);
                if (i <= 0 || i >= static_cast<int>(g_mics.size()))
                    return 0;
                const LONG err = Settings::SetDeviceCustom(SelectedGuid(), Checked(IDC_CUSTOM), g_mics[static_cast<size_t>(i)].name.c_str());
                PopulateDeviceCombo();  // keeps the same microphone selected (by id), or the defaults if it left the list
                LoadUi();
                Status(err == ERROR_SUCCESS ? L"Saved." : L"Could not save (run the install script once as administrator).");
            }
            else if (id == IDC_DEFAULTS)
            {
                Settings d{};
                d.bypass = 0;  // defaults also clear a scripted A/B bypass
                const LONG err = Settings::WriteToRegistry(d, Scope());
                LoadUi();
                Status(err == ERROR_SUCCESS ? L"Defaults restored." : L"Could not save settings.");
            }
            else if ((code == BN_CLICKED && id > IDC_FIRST && id != IDC_CUSTOM) || (code == CBN_SELCHANGE && id != IDC_DEVICE))
                SaveFromUi(id);
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);  // keep running in the tray
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
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

void app::ShowMainWindow()
{
    PopulateDeviceCombo();
    LoadUi();
    ShowWindow(g_wnd, SW_RESTORE);
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
}

void app::ExitApp()
{
    tray::Shutdown();
    DestroyWindow(g_wnd);  // WM_DESTROY -> PostQuitMessage
}

// "WebrtcAudioEngineSettings.exe"        : tray + open the settings window
// "WebrtcAudioEngineSettings.exe /tray"  : tray only (started at sign-in)
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int show)
{
    const bool trayOnly = cmdLine && wcsstr(cmdLine, L"/tray") != nullptr;
    g_showMsg = RegisterWindowMessageW(L"WebrtcAudioEngine.ShowSettings");

    // Single instance: a second launch asks the running one to show its window.
    // ERROR_ACCESS_DENIED: the running copy is elevated, so its mutex is not ours to open, but it exists.
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"WebrtcAudioEngineSettings.SingleInstance");
    const DWORD mutexErr = GetLastError();
    if (mutexErr == ERROR_ALREADY_EXISTS || (!instanceMutex && mutexErr == ERROR_ACCESS_DENIED))
    {
        if (!trayOnly)
            if (HWND other = FindWindowW(L"WebrtcAudioEngineSettings", nullptr))
            {
                AllowSetForegroundWindow(ASFW_ANY);
                PostMessageW(other, g_showMsg, 0, 0);
            }
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // device enumeration (MMDevice API)
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"WebrtcAudioEngineSettings";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);  // (window icon; the tray icon is drawn in TrayFlyout.cpp)
    RegisterClassExW(&wc);

    g_dpi = GetDpiForSystem();
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_dpi);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    g_bold = CreateFontIndirectW(&bold);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, S(400), S(690)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, g_dpi);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"WebRTC Audio Engine Settings", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    ChangeWindowMessageFilterEx(g_wnd, g_showMsg, MSGFLT_ALLOW, nullptr);  // a non-elevated second launch may ask us to show
    BuildUi();
    PopulateDeviceCombo();
    LoadUi();
    Status(L"Changes apply immediately.");
    tray::Init(inst);
    if (!trayOnly)
        ShowWindow(g_wnd, show);

    // Refresh when the settings change elsewhere (scripts, another tool), including the per-microphone
    // subkeys. If the key does not exist yet, look again every few seconds.
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
        Status(L"Not installed: the settings key is missing (run the install script).");

    MSG m;
    for (;;)
    {
        const DWORD r = MsgWaitForMultipleObjects(1, &changed, FALSE, key ? INFINITE : 5000, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0 || (r == WAIT_TIMEOUT && watch()))
        {
            if (!(Settings::ReadFromRegistry(Scope()) == g_current))
                LoadUi();
            tray::SettingsChanged();
            if (r == WAIT_OBJECT_0) watch();
            else Status(L"Changes apply immediately.");  // the settings key showed up
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
