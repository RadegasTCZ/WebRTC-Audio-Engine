// SPDX-License-Identifier: GPL-3.0-or-later
// "/install" and "/uninstall" modes of the settings app (no window).
//
// /install [version]: run by Windows (as SYSTEM, from the driver store) when the driver package's software
// component installs, or by Install-SettingsApp.ps1 from an admin shell. Copies this exe to Program Files,
// adds the Start-menu shortcut, the tray icon at sign-in (HKLM Run), an Apps & features entry, prepares
// the settings key (Users may change values) and starts the tray icon for whoever is signed in.
// /uninstall: from Apps & features (asks for admin) or the uninstall scripts. Undoes all of that except
// the settings themselves.
// A short log goes to %ProgramData%\WebrtcAudioEngine\setup.log.
#include "App.h"
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include "../src/Settings.h"

namespace
{
    constexpr wchar_t kExeName[] = L"WebrtcAudioEngineSettings.exe";
    constexpr wchar_t kDirName[] = L"WebrtcAudioEngine";
    constexpr wchar_t kShortcut[] = L"WebRTC Audio Engine Settings.lnk";
    constexpr wchar_t kRunKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kRunName[] = L"WebRTC Audio Engine";
    constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WebrtcAudioEngine";

    std::wstring Known(REFKNOWNFOLDERID id)
    {
        PWSTR p = nullptr;
        std::wstring s;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
        CoTaskMemFree(p);
        return s;
    }
    std::wstring InstallDir() { return Known(FOLDERID_ProgramFiles) + L"\\" + kDirName; }
    std::wstring InstalledExe() { return InstallDir() + L"\\" + kExeName; }
    std::wstring ShortcutPath() { return Known(FOLDERID_CommonPrograms) + L"\\" + kShortcut; }
    std::wstring SelfPath()
    {
        wchar_t p[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, p, ARRAYSIZE(p));
        return std::wstring(p, n);
    }

    void Log(const wchar_t* fmt, ...)
    {
        const std::wstring dir = Known(FOLDERID_ProgramData) + L"\\" + kDirName;
        CreateDirectoryW(dir.c_str(), nullptr);
        FILE* f = nullptr;
        if (_wfopen_s(&f, (dir + L"\\setup.log").c_str(), L"a, ccs=UTF-8") != 0 || !f) return;
        SYSTEMTIME t;
        GetLocalTime(&t);
        fwprintf(f, L"%04u-%02u-%02u %02u:%02u:%02u  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        va_list a;
        va_start(a, fmt);
        vfwprintf(f, fmt, a);
        va_end(a);
        fputwc(L'\n', f);
        fclose(f);
    }

    bool Elevated()
    {
        HANDLE tok = nullptr;
        TOKEN_ELEVATION e{};
        DWORD n = 0;
        const bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok) &&
                        GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
        if (tok) CloseHandle(tok);
        return ok;
    }

    // Closes every other running copy of the app: politely in this session (tray icon removed cleanly),
    // then by force (copies in other sessions, e.g. when Windows runs us as SYSTEM).
    bool RunInUserSessions(const std::wstring& exe, const wchar_t* args, DWORD waitMs);
    void CloseRunningApps()
    {
        // As SYSTEM (session 0) we cannot post to windows on the users' desktops: run "/quit" there instead.
        if (!RunInUserSessions(SelfPath(), L"/quit", 3000))
            setup::Quit();
        const DWORD self = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        {
            if (pe.th32ProcessID == self || _wcsicmp(pe.szExeFile, kExeName) != 0) continue;
            HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
            if (!p) continue;
            if (WaitForSingleObject(p, 2000) == WAIT_TIMEOUT)
            {
                TerminateProcess(p, 0);
                WaitForSingleObject(p, 5000);
                Log(L"closed running copy (pid %lu) by force", pe.th32ProcessID);
            }
            CloseHandle(p);
        }
        CloseHandle(snap);
    }

    LONG SetString(HKEY k, const wchar_t* name, const std::wstring& v)
    {
        return RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    }
    LONG SetDword(HKEY k, const wchar_t* name, DWORD v) { return RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v)); }

    bool MakeShortcut(const std::wstring& target)
    {
        IShellLinkW* link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
        link->SetPath(target.c_str());
        link->SetDescription(L"Echo cancellation, noise suppression, automatic gain and cleanup for your microphone");
        IPersistFile* file = nullptr;
        bool ok = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Save(ShortcutPath().c_str(), TRUE));
        if (file) file->Release();
        link->Release();
        return ok;
    }

    // HKLM\SOFTWARE\WebrtcVoiceApo (+ Devices): Users, the audio engine (LOCAL SERVICE) and app containers
    // (Windows Settings toggles our effects through the APO) may change values and add microphone subkeys.
    // Same rights as Set-VoiceSettings.ps1 grants.
    LONG PrepareSettingsKey()
    {
        HKEY k = nullptr;
        LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, settings::kRegistryPath, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &k, nullptr);
        if (r != ERROR_SUCCESS) return r;
        PACL dacl = nullptr, newDacl = nullptr;
        PSECURITY_DESCRIPTOR sd = nullptr;
        r = static_cast<LONG>(GetSecurityInfo(k, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd));
        if (r == ERROR_SUCCESS)
        {
            const wchar_t* sids[] = {L"S-1-5-32-545", L"S-1-5-19", L"S-1-15-2-1", L"S-1-15-2-2"};
            EXPLICIT_ACCESSW ea[4]{};
            PSID psid[4]{};
            for (int i = 0; i < 4; ++i)
            {
                ConvertStringSidToSidW(sids[i], &psid[i]);
                ea[i].grfAccessPermissions = KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_CREATE_SUB_KEY | KEY_ENUMERATE_SUB_KEYS | KEY_NOTIFY | KEY_READ;
                ea[i].grfAccessMode = GRANT_ACCESS;
                ea[i].grfInheritance = SUB_CONTAINERS_ONLY_INHERIT;
                ea[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
                ea[i].Trustee.ptstrName = static_cast<LPWSTR>(psid[i]);
            }
            r = static_cast<LONG>(SetEntriesInAclW(4, ea, dacl, &newDacl));
            if (r == ERROR_SUCCESS)
                r = static_cast<LONG>(SetSecurityInfo(k, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr, newDacl, nullptr));
            for (PSID s : psid) LocalFree(s);
            LocalFree(newDacl);
            LocalFree(sd);
        }
        HKEY dev = nullptr;
        if (RegCreateKeyExW(k, L"Devices", 0, nullptr, 0, KEY_READ, nullptr, &dev, nullptr) == ERROR_SUCCESS) RegCloseKey(dev);
        RegCloseKey(k);
        return r;
    }

    // Runs "exe args" in every signed-in session as that user (not elevated), optionally waiting for it.
    // Needs SYSTEM rights (Windows running us from the driver package); returns false without them.
    bool RunInUserSessions(const std::wstring& exe, const wchar_t* args, DWORD waitMs)
    {
        WTS_SESSION_INFOW* sessions = nullptr;
        DWORD count = 0;
        bool system = false;
        if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) return false;
        for (DWORD i = 0; i < count; ++i)
        {
            if (sessions[i].State != WTSActive) continue;
            HANDLE user = nullptr;
            if (!WTSQueryUserToken(sessions[i].SessionId, &user))
            {
                if (GetLastError() == ERROR_PRIVILEGE_NOT_HELD) break;  // not SYSTEM
                continue;
            }
            system = true;
            void* env = nullptr;
            CreateEnvironmentBlock(&env, user, FALSE);
            std::wstring cmd = L"\"" + exe + L"\" " + args;
            STARTUPINFOW si{sizeof(si)};
            si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
            PROCESS_INFORMATION pi{};
            if (CreateProcessAsUserW(user, exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env, nullptr, &si, &pi))
            {
                Log(L"ran %s in session %lu", args, sessions[i].SessionId);
                if (waitMs) WaitForSingleObject(pi.hProcess, waitMs);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            else
                Log(L"could not run %s in session %lu (error %lu)", args, sessions[i].SessionId, GetLastError());
            if (env) DestroyEnvironmentBlock(env);
            CloseHandle(user);
        }
        WTSFreeMemory(sessions);
        return system;
    }

    // Starts the tray icon for the signed-in users; without SYSTEM rights (admin shell) in this session.
    void StartTray(const std::wstring& exe)
    {
        if (RunInUserSessions(exe, L"/tray", 0)) return;
        std::wstring cmd = L"\"" + exe + L"\" /tray";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    // The word after the switch, e.g. "/install 0.14.320.123" -> "0.14.320.123".
    std::wstring Argument(const wchar_t* cmdLine, const wchar_t* sw)
    {
        const wchar_t* p = wcsstr(cmdLine, sw);
        if (!p) return L"";
        p += wcslen(sw);
        while (*p == L' ') ++p;
        std::wstring v;
        while (*p && *p != L' ' && *p != L'/') v += *p++;
        return v;
    }
}

namespace setup
{
    // "/quit": asks the running app in this session to exit (removes its tray icon cleanly).
    int Quit()
    {
        const UINT quit = RegisterWindowMessageW(L"WebrtcAudioEngine.Quit");
        if (HWND w = FindWindowW(L"WebrtcAudioEngineSettings", nullptr)) PostMessageW(w, quit, 0, 0);
        return 0;
    }

    int Install(const wchar_t* cmdLine)
    {
        const std::wstring version = Argument(cmdLine, L"/install");
        Log(L"install %s from %s", version.empty() ? L"(no version)" : version.c_str(), SelfPath().c_str());
        if (!Elevated()) { Log(L"failed: needs admin rights"); return ERROR_ACCESS_DENIED; }
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        CloseRunningApps();
        const std::wstring dir = InstallDir(), exe = InstalledExe(), self = SelfPath();
        CreateDirectoryW(dir.c_str(), nullptr);
        if (_wcsicmp(self.c_str(), exe.c_str()) != 0)
        {
            BOOL copied = FALSE;
            for (int i = 0; i < 20 && !(copied = CopyFileW(self.c_str(), exe.c_str(), FALSE)); ++i)
                Sleep(250);  // a closed copy can keep the file locked for a moment
            if (!copied)
            {
                const DWORD e = GetLastError();
                Log(L"failed: could not copy to %s (error %lu)", exe.c_str(), e);
                return static_cast<int>(e);
            }
        }

        if (!MakeShortcut(exe)) Log(L"warning: Start-menu shortcut not created");
        HKEY k = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS)
        {
            SetString(k, kRunName, L"\"" + exe + L"\" /tray");
            RegCloseKey(k);
        }
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS)
        {
            SetString(k, L"DisplayName", L"WebRTC Audio Engine Settings");
            if (!version.empty()) SetString(k, L"DisplayVersion", version);
            SetString(k, L"Publisher", L"RadegasTCZ");
            SetString(k, L"DisplayIcon", exe + L",0");
            SetString(k, L"InstallLocation", dir);
            SetString(k, L"UninstallString", L"\"" + exe + L"\" /uninstall");
            SetDword(k, L"NoModify", 1);
            SetDword(k, L"NoRepair", 1);
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &fa)) SetDword(k, L"EstimatedSize", fa.nFileSizeLow / 1024 + 1);
            RegCloseKey(k);
        }
        if (const LONG r = PrepareSettingsKey(); r != ERROR_SUCCESS) Log(L"warning: settings key not prepared (error %ld)", r);

        StartTray(exe);
        Log(L"installed %s", exe.c_str());
        return 0;
    }

    int Uninstall(const wchar_t* cmdLine)
    {
        if (!Elevated())
        {
            // From Apps & features: ask for admin rights and run again.
            const std::wstring self = SelfPath();
            SHELLEXECUTEINFOW sei{sizeof(sei)};
            sei.fMask = SEE_MASK_NOCLOSEPROCESS;
            sei.lpVerb = L"runas";
            sei.lpFile = self.c_str();
            sei.lpParameters = L"/uninstall";
            sei.nShow = SW_HIDE;
            if (!ShellExecuteExW(&sei)) return static_cast<int>(GetLastError());
            DWORD code = 1;
            if (sei.hProcess)
            {
                WaitForSingleObject(sei.hProcess, INFINITE);
                GetExitCodeProcess(sei.hProcess, &code);
                CloseHandle(sei.hProcess);
            }
            return static_cast<int>(code);
        }
        (void)cmdLine;
        Log(L"uninstall from %s", SelfPath().c_str());
        CloseRunningApps();
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS)
        {
            RegDeleteValueW(k, kRunName);
            RegCloseKey(k);
        }
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, kUninstallKey);
        DeleteFileW(ShortcutPath().c_str());

        // The folder holds the running exe when started from Apps & features: delete it once we have exited.
        const std::wstring dir = InstallDir();
        if (DeleteFileW(InstalledExe().c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND)
            RemoveDirectoryW(dir.c_str());
        else
        {
            wchar_t sys[MAX_PATH];
            GetSystemDirectoryW(sys, MAX_PATH);
            const std::wstring cmdExe = std::wstring(sys) + L"\\cmd.exe";
            std::wstring cmd = L"cmd.exe /d /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + dir + L"\"";
            STARTUPINFOW si{sizeof(si)};
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(cmdExe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, sys, &si, &pi))
            {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
        Log(L"uninstalled (settings kept)");
        return 0;
    }
}
