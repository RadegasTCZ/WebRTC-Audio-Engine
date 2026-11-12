// SPDX-License-Identifier: GPL-3.0-or-later
// "/install" and "/uninstall" modes of the settings app (no window).
//
// /install [version]: run by Windows (as SYSTEM, from the driver store) when the driver package's software
// component installs, or by Install-SettingsApp.ps1 from an admin shell. Copies this exe to Program Files,
// adds the Start-menu shortcut, the tray icon at sign-in (HKLM Run), an Apps & features entry, prepares
// the settings key (Users may change values) and starts the tray icon for the signed-in users.
// /uninstall: from Apps & features (asks for admin rights) or the scripts. Undoes all of that except the
// settings. The running exe cannot be deleted, so it is moved into the protected log folder and deleted
// at the next restart (never copied to a user-writable folder and run elevated from there).
//
// Closing running copies: the installer bumps AppGeneration in HKLM\SOFTWARE\WebrtcAudioEngine (only
// admins may write it); every running copy watches that key (all sessions, any elevation) and exits
// when it changes. Copies too old to know this are ended by force as a last resort.
// A short log goes to %ProgramData%\WebrtcAudioEngine\setup.log (folder writable by SYSTEM and admins only).
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
#include <vector>
#include "../src/Settings.h"

namespace
{
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
    std::wstring InstalledExe() { return InstallDir() + L"\\" + app::kExeName; }
    std::wstring ShortcutPath() { return Known(FOLDERID_CommonPrograms) + L"\\" + kShortcut; }
    std::wstring SelfPath()
    {
        wchar_t p[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, p, ARRAYSIZE(p));
        return std::wstring(p, n);
    }
    std::wstring Quoted(const std::wstring& s) { return L"\"" + s + L"\""; }

    bool TokenIs(WELL_KNOWN_SID_TYPE type)
    {
        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
        BYTE buf[256];
        DWORD n = 0;
        const bool is = GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &n) &&
                        IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, type);
        CloseHandle(tok);
        return is;
    }
    bool IsSystem() { return TokenIs(WinLocalSystemSid); }

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

    // ------------------------------------------------------------------ log

    // True if `path` is owned by SYSTEM or Administrators (so no ordinary user prepared it for us).
    bool OwnedByAdmins(const std::wstring& path)
    {
        PSID owner = nullptr;
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS)
            return false;
        const bool ok = IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid);
        LocalFree(sd);
        return ok;
    }

    // %ProgramData%\WebrtcAudioEngine, writable by SYSTEM and Administrators only, or empty if that cannot be
    // guaranteed (then nothing is logged). We write there as SYSTEM: a folder an ordinary user could create
    // or write into would let them redirect the log onto any file (links), like Initialize-LogDir guards against.
    const std::wstring& LogDir()
    {
        static std::wstring dir = [] {
            const std::wstring d = Known(FOLDERID_ProgramData) + L"\\" + kDirName;
            PSECURITY_DESCRIPTOR sd = nullptr;
            // Protected DACL: SYSTEM and Administrators full control, Users read.
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)",
                                                                       SDDL_REVISION_1, &sd, nullptr))
                return std::wstring();
            SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
            std::wstring result;
            if (CreateDirectoryW(d.c_str(), &sa))
                result = d;
            else if (GetLastError() == ERROR_ALREADY_EXISTS)
            {
                const DWORD attr = GetFileAttributesW(d.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT) &&
                    OwnedByAdmins(d))
                {
                    // Re-apply the ACL (an older build left the inherited ProgramData one: Users could add files).
                    BOOL present = FALSE, defaulted = FALSE;
                    PACL dacl = nullptr;
                    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
                    if (SetNamedSecurityInfoW(const_cast<LPWSTR>(d.c_str()), SE_FILE_OBJECT,
                                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl,
                                              nullptr) == ERROR_SUCCESS)
                    {
                        // A log a user planted earlier is not ours: start a new one.
                        const std::wstring log = d + L"\\setup.log";
                        if (GetFileAttributesW(log.c_str()) != INVALID_FILE_ATTRIBUTES && !OwnedByAdmins(log)) DeleteFileW(log.c_str());
                        result = d;
                    }
                }
            }
            LocalFree(sd);
            return result;
        }();
        return dir;
    }

    void Log(const wchar_t* fmt, ...)
    {
        if (LogDir().empty()) return;
        const std::wstring path = LogDir() + L"\\setup.log";
        HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        wchar_t line[1024];
        SYSTEMTIME t;
        GetLocalTime(&t);
        int n = swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        va_list a;
        va_start(a, fmt);
        const int m = _vsnwprintf_s(line + n, ARRAYSIZE(line) - n - 2, _TRUNCATE, fmt, a);
        va_end(a);
        n += m < 0 ? static_cast<int>(wcslen(line + n)) : m;
        line[n++] = L'\r';
        line[n++] = L'\n';
        char utf8[3 * ARRAYSIZE(line)];
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, n, utf8, sizeof(utf8), nullptr, nullptr);
        DWORD written = 0;
        WriteFile(h, utf8, static_cast<DWORD>(bytes), &written, nullptr);
        CloseHandle(h);
    }

    // ------------------------------------------------------------------ registry

    LONG SetString(HKEY k, const wchar_t* name, const std::wstring& v)
    {
        return RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    }
    LONG SetDword(HKEY k, const wchar_t* name, DWORD v) { return RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v)); }

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

    // ------------------------------------------------------------------ running copies

    struct Proc
    {
        DWORD pid;
        HANDLE handle;
    };

    // Other running copies of the app, except us and the process that started us (the non-elevated
    // "/uninstall" from Apps & features that handed over to this copy).
    std::vector<Proc> OtherCopies(DWORD access)
    {
        std::vector<Proc> list;
        const DWORD self = GetCurrentProcessId();
        DWORD parent = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return list;
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            if (pe.th32ProcessID == self) parent = pe.th32ParentProcessID;
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        {
            if (pe.th32ProcessID == self || pe.th32ProcessID == parent || _wcsicmp(pe.szExeFile, app::kExeName) != 0) continue;
            if (HANDLE h = OpenProcess(access, FALSE, pe.th32ProcessID)) list.push_back({pe.th32ProcessID, h});
            else if (GetLastError() != ERROR_INVALID_PARAMETER)  // that one: it has just exited
                Log(L"cannot open running copy (pid %lu, error %lu)", pe.th32ProcessID, GetLastError());
        }
        CloseHandle(snap);
        return list;
    }

    // Asks every running copy to exit (AppGeneration changes; they watch the app key) and waits for
    // them; copies that do not react within 5 s (builds before AppGeneration) are ended by force.
    void CloseRunningApps()
    {
        HKEY k = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, app::kAppKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS)
        {
            SetDword(k, app::kGenerationValue, app::AppGeneration() + 1);
            RegCloseKey(k);
        }
        std::vector<Proc> copies = OtherCopies(PROCESS_TERMINATE | SYNCHRONIZE);
        const ULONGLONG deadline = GetTickCount64() + 5000;
        for (Proc& p : copies)
        {
            const ULONGLONG now = GetTickCount64();
            if (WaitForSingleObject(p.handle, now < deadline ? static_cast<DWORD>(deadline - now) : 0) == WAIT_TIMEOUT)
            {
                TerminateProcess(p.handle, 0);
                WaitForSingleObject(p.handle, 5000);
                Log(L"closed running copy (pid %lu) by force", p.pid);
            }
            CloseHandle(p.handle);
        }
    }

    // ------------------------------------------------------------------ starting the tray

    void Launch(HANDLE token, const std::wstring& exe, const std::wstring& args, const wchar_t* who)
    {
        std::wstring cmd = Quoted(exe) + L" " + args;
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        BOOL ok = FALSE;
        if (IsSystem())
        {
            void* env = nullptr;
            CreateEnvironmentBlock(&env, token, FALSE);
            si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
            ok = CreateProcessAsUserW(token, exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env, nullptr, &si, &pi);
            if (env) DestroyEnvironmentBlock(env);
        }
        else if (token)
            ok = CreateProcessWithTokenW(token, 0, exe.c_str(), cmd.data(), 0, nullptr, nullptr, &si, &pi);
        else
            ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
        if (!ok)
        {
            Log(L"could not start the tray %s (error %lu)", who, GetLastError());
            return;
        }
        Log(L"tray started %s", who);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    // Starts the tray icon for the signed-in users, as them (not elevated). As SYSTEM: in every session
    // with a user (also switched-away ones); nobody signed in: nothing, the Run entry starts it at sign-in.
    // From an admin shell: as the desktop user (Explorer's token), else with our own token.
    void StartTray(const std::wstring& exe)
    {
        if (IsSystem())
        {
            WTS_SESSION_INFOW* sessions = nullptr;
            DWORD count = 0;
            if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) return;
            for (DWORD i = 0; i < count; ++i)
            {
                if (sessions[i].State != WTSActive && sessions[i].State != WTSDisconnected) continue;
                HANDLE user = nullptr;
                if (!WTSQueryUserToken(sessions[i].SessionId, &user)) continue;  // no user in this session
                wchar_t who[32];
                swprintf_s(who, L"in session %lu", sessions[i].SessionId);
                Launch(user, exe, L"/tray", who);
                CloseHandle(user);
            }
            WTSFreeMemory(sessions);
            return;
        }
        HANDLE desktopUser = nullptr;
        DWORD pid = 0;
        if (HWND shell = GetShellWindow()) GetWindowThreadProcessId(shell, &pid);
        if (HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr)
        {
            HANDLE t = nullptr;
            if (OpenProcessToken(p, TOKEN_DUPLICATE, &t))
            {
                DuplicateTokenEx(t, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
                                 nullptr, SecurityImpersonation, TokenPrimary, &desktopUser);
                CloseHandle(t);
            }
            CloseHandle(p);
        }
        Launch(desktopUser, exe, L"/tray", desktopUser ? L"as the desktop user" : L"in this session");
        if (desktopUser) CloseHandle(desktopUser);
    }

    // The word after the switch, e.g. "/install 0.14.320.123" -> "0.14.320.123" (quotes dropped).
    std::wstring Argument(const wchar_t* cmdLine, const wchar_t* sw)
    {
        const wchar_t* p = wcsstr(cmdLine, sw);
        if (!p) return L"";
        p += wcslen(sw);
        while (*p == L' ' || *p == L'"') ++p;
        std::wstring v;
        while (*p && *p != L' ' && *p != L'/' && *p != L'"') v += *p++;
        return v;
    }

    // Runs `exe args` (asking for admin rights if `elevate`), optionally waiting for its exit code.
    DWORD Run(const std::wstring& exe, const wchar_t* args, bool elevate, bool wait)
    {
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = elevate ? L"runas" : nullptr;
        sei.lpFile = exe.c_str();
        sei.lpParameters = args;
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei)) return GetLastError();
        DWORD code = 0;
        if (sei.hProcess)
        {
            if (wait)
            {
                WaitForSingleObject(sei.hProcess, INFINITE);
                GetExitCodeProcess(sei.hProcess, &code);
            }
            CloseHandle(sei.hProcess);
        }
        return code;
    }
}

namespace
{
    // Deletes a file that may still be running (an exe in use cannot be deleted, but it can be moved):
    // moves it into the protected log folder and deletes it at the next restart. True if it is gone
    // from `path`.
    bool RemoveFile(const std::wstring& path)
    {
        if (DeleteFileW(path.c_str())) return true;
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return true;
        if (!LogDir().empty())
        {
            const std::wstring parked = LogDir() + L"\\removed-" + std::to_wstring(GetTickCount64()) + L".exe";
            if (MoveFileExW(path.c_str(), parked.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                MoveFileExW(parked.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
                return true;
            }
            err = GetLastError();
        }
        Log(L"could not remove %s (error %lu)", path.c_str(), err);
        SetLastError(err);
        return false;
    }

    // Exes parked by RemoveFile that are no longer running.
    void CleanParked()
    {
        if (LogDir().empty()) return;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((LogDir() + L"\\removed-*.exe").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) return;
        do DeleteFileW((LogDir() + L"\\" + fd.cFileName).c_str());
        while (FindNextFileW(f, &fd));
        FindClose(f);
    }
}

DWORD app::AppGeneration()
{
    DWORD v = 0, size = sizeof(v);
    RegGetValueW(HKEY_LOCAL_MACHINE, kAppKey, kGenerationValue, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}

namespace setup
{
    int Install(const wchar_t* cmdLine)
    {
        if (!Elevated()) return ERROR_ACCESS_DENIED;  // before any logging: only admins/SYSTEM write the log
        const std::wstring version = Argument(cmdLine, L"/install");
        Log(L"install %s from %s%s", version.empty() ? L"(no version)" : version.c_str(), SelfPath().c_str(), IsSystem() ? L" (SYSTEM)" : L"");
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        if (const LONG r = PrepareSettingsKey(); r != ERROR_SUCCESS) Log(L"warning: settings key not prepared (error %ld)", r);
        CleanParked();
        CloseRunningApps();
        const std::wstring dir = InstallDir(), exe = InstalledExe(), self = SelfPath();
        CreateDirectoryW(dir.c_str(), nullptr);
        if (_wcsicmp(self.c_str(), exe.c_str()) != 0)
        {
            BOOL copied = FALSE;
            for (int i = 0; i < 20 && !(copied = CopyFileW(self.c_str(), exe.c_str(), FALSE)); ++i)
            {
                if (i == 8) RemoveFile(exe);  // still in use after 2 s (a copy we could not close): move it aside
                Sleep(250);                   // a closed copy can keep the file locked for a moment
            }
            if (!copied)
            {
                const DWORD e = GetLastError();
                Log(L"failed: could not copy to %s (error %lu)", exe.c_str(), e);
                if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) StartTray(exe);  // give the users their tray back
                return static_cast<int>(e);
            }
        }

        IShellLinkW* link = nullptr;
        bool shortcut = false;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        {
            link->SetPath(exe.c_str());
            link->SetDescription(L"Echo cancellation, noise suppression, automatic gain and cleanup for your microphone");
            IPersistFile* file = nullptr;
            shortcut = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Save(ShortcutPath().c_str(), TRUE));
            if (file) file->Release();
            link->Release();
        }
        if (!shortcut) Log(L"warning: Start-menu shortcut not created");

        HKEY k = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS)
        {
            SetString(k, kRunName, Quoted(exe) + L" /tray");
            RegCloseKey(k);
        }
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS)
        {
            SetString(k, L"DisplayName", L"WebRTC Audio Engine Settings");
            if (!version.empty()) SetString(k, L"DisplayVersion", version);
            SetString(k, L"Publisher", L"RadegasTCZ");
            SetString(k, L"DisplayIcon", exe + L",0");
            SetString(k, L"InstallLocation", dir);
            SetString(k, L"UninstallString", Quoted(exe) + L" /uninstall");
            SetDword(k, L"NoModify", 1);
            SetDword(k, L"NoRepair", 1);
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &fa)) SetDword(k, L"EstimatedSize", fa.nFileSizeLow / 1024 + 1);
            RegCloseKey(k);
        }

        StartTray(exe);
        Log(L"installed %s", exe.c_str());
        return 0;
    }

    int Uninstall()
    {
        const std::wstring self = SelfPath();
        // From Apps & features: not elevated. Ask for admin rights and run this same exe (in Program Files,
        // which only admins can change) again.
        if (!Elevated()) return static_cast<int>(Run(self, L"/uninstall", true, true));

        Log(L"uninstall from %s", self.c_str());
        CloseRunningApps();
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS)
        {
            RegDeleteValueW(k, kRunName);
            RegCloseKey(k);
        }
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, kUninstallKey);
        DeleteFileW(ShortcutPath().c_str());

        // We (and the Apps & features process waiting for us) may be running from this exe: RemoveFile moves it
        // out of the way and deletes it at the next restart.
        const std::wstring exe = InstalledExe();
        if (!RemoveFile(exe))
        {
            const DWORD err = GetLastError();
            Log(L"failed: could not remove %s (error %lu)", exe.c_str(), err);
            return static_cast<int>(err ? err : ERROR_ACCESS_DENIED);
        }
        RemoveDirectoryW(InstallDir().c_str());
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, app::kAppKey);
        Log(L"uninstalled (settings kept)");
        return 0;
    }
}
