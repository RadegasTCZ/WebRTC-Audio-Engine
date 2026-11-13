// MSI custom actions for the WebRTC Audio Engine installer (installer/Product.wxs).
//
// The MSI copies the three signed driver packages to <Program Files>\WebrtcAudioEngine\Driver\ and then calls:
//   InstallDrivers   (deferred, as SYSTEM): trust the signing certificate, remove the old log folder, stop
//                    Windows Audio, install component -> app -> extension, start Windows Audio, remove superseded
//                    versions of our packages.
//   UninstallDrivers (deferred, as SYSTEM): stop Windows Audio, uninstall extension -> component -> app, remove
//                    their software devices, start Windows Audio, remove the settings app (it installed itself
//                    from the app package), take back the certificate trust we added. Settings are kept.
//   RemoveLogs       (deferred, as SYSTEM, only on a real uninstall, not during an upgrade): the effect's logs.
// Each is also the other's rollback action. CustomActionData = the Driver folder (with a trailing backslash).
//
// This is the C++ form of scripts\Install-Package.ps1 / Uninstall-Package.ps1. It uses the driver APIs instead
// of parsing pnputil's output, which is translated on non-English Windows.

#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <setupapi.h>
#include <newdev.h>
#include <wincrypt.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#include <cstdarg>
#include <cwchar>
#include <string>
#include <vector>

namespace
{
    constexpr const wchar_t* kInfs[] = {L"WebrtcAudioEngine.inf", L"WebrtcAudioEngine-App.inf",
                                        L"WebrtcAudioEngine-Extension.inf"};  // install order
    constexpr const wchar_t* kInfDirs[] = {L"component", L"app", L"extension"};
    constexpr const wchar_t* kHardwareIds[] = {L"SWC\\VEN_WRTC&AUDIO_EFFECTPACK_WEBRTCENGINE",
                                               L"SWC\\VEN_WRTC&WEBRTCENGINE_SETTINGSAPP"};
    constexpr wchar_t kCertFile[] = L"WebrtcAudioEngine.cer";
    // Which certificate stores we added the certificate to (so uninstall only takes back our own trust).
    constexpr wchar_t kSetupStateKey[] = L"SOFTWARE\\WebrtcAudioEngineSetup\\State";

    MSIHANDLE g_session = 0;

    // Writes a line to the MSI log (msiexec /l*v).
    void Log(const wchar_t* fmt, ...)
    {
        wchar_t text[1024];
        va_list args;
        va_start(args, fmt);
        _vsnwprintf_s(text, _TRUNCATE, fmt, args);
        va_end(args);
        PMSIHANDLE rec = MsiCreateRecord(1);
        MsiRecordSetStringW(rec, 0, L"WebrtcSetup: [1]");
        MsiRecordSetStringW(rec, 1, text);
        MsiProcessMessage(g_session, INSTALLMESSAGE_INFO, rec);
    }

    std::wstring CustomActionData()
    {
        DWORD n = 0;
        wchar_t empty[1] = L"";
        MsiGetPropertyW(g_session, L"CustomActionData", empty, &n);
        std::wstring s(++n, L'\0');
        if (MsiGetPropertyW(g_session, L"CustomActionData", s.data(), &n) != ERROR_SUCCESS) return {};
        s.resize(n);
        if (!s.empty() && s.back() != L'\\') s += L'\\';
        return s;
    }

    std::wstring WindowsDir()
    {
        wchar_t w[MAX_PATH];
        const UINT n = GetWindowsDirectoryW(w, MAX_PATH);
        return (n && n < MAX_PATH) ? std::wstring(w, n) : std::wstring(L"C:\\Windows");
    }

    std::wstring KnownFolder(REFKNOWNFOLDERID id)
    {
        PWSTR p = nullptr;
        std::wstring s;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
        CoTaskMemFree(p);
        return s;
    }

    // ------------------------------------------------------------------ driver store

    struct OemInf
    {
        std::wstring published;  // oemNN.inf
        std::wstring original;   // our INF name
    };

    // Our packages in the driver store, found by their original INF name.
    std::vector<OemInf> OurPackages()
    {
        std::vector<OemInf> list;
        const std::wstring infDir = WindowsDir() + L"\\INF\\";
        WIN32_FIND_DATAW fd{};
        HANDLE f = FindFirstFileW((infDir + L"oem*.inf").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) return list;
        do
        {
            const std::wstring path = infDir + fd.cFileName;
            DWORD size = 0;
            if (!SetupGetInfInformationW(path.c_str(), INFINFO_INF_NAME_IS_ABSOLUTE, nullptr, 0, &size) || !size) continue;
            std::vector<BYTE> buf(size);
            auto* info = reinterpret_cast<PSP_INF_INFORMATION>(buf.data());
            if (!SetupGetInfInformationW(path.c_str(), INFINFO_INF_NAME_IS_ABSOLUTE, info, size, nullptr)) continue;
            SP_ORIGINAL_FILE_INFO_W ofi{};
            ofi.cbSize = sizeof(ofi);
            if (!SetupQueryInfOriginalFileInformationW(info, 0, nullptr, &ofi)) continue;
            for (const wchar_t* ours : kInfs)
                if (_wcsicmp(ofi.OriginalInfName, ours) == 0) list.push_back({fd.cFileName, ours});
        } while (FindNextFileW(f, &fd));
        FindClose(f);
        return list;
    }

    bool Contains(const std::vector<OemInf>& list, const std::wstring& published)
    {
        for (const auto& p : list)
            if (_wcsicmp(p.published.c_str(), published.c_str()) == 0) return true;
        return false;
    }

    // ------------------------------------------------------------------ Windows Audio

    bool WaitState(SC_HANDLE svc, DWORD state)
    {
        for (int i = 0; i < 300; ++i)  // 30 s
        {
            SERVICE_STATUS st{};
            if (!QueryServiceStatus(svc, &st)) return false;
            if (st.dwCurrentState == state) return true;
            Sleep(100);
        }
        return false;
    }

    bool StopService(SC_HANDLE scm, const wchar_t* name)
    {
        SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
        if (!svc) return false;
        SERVICE_STATUS st{};
        bool ok = ControlService(svc, SERVICE_CONTROL_STOP, &st) || GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
        if (!ok) Log(L"could not stop %s (error %lu)", name, GetLastError());
        ok = ok && WaitState(svc, SERVICE_STOPPED);
        CloseServiceHandle(svc);
        return ok;
    }

    // Stops Windows Audio and the running services that depend on it (e.g. Realtek / Dolby / Nahimic services),
    // like Stop-Service -Force. Returns the dependents, to be started again by StartAudio.
    std::vector<std::wstring> StopAudio()
    {
        std::vector<std::wstring> dependents;
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) return dependents;
        if (SC_HANDLE audio = OpenServiceW(scm, L"Audiosrv", SERVICE_ENUMERATE_DEPENDENTS))
        {
            DWORD bytes = 0, count = 0;
            EnumDependentServicesW(audio, SERVICE_ACTIVE, nullptr, 0, &bytes, &count);
            if (bytes)
            {
                std::vector<BYTE> buf(bytes);
                auto* list = reinterpret_cast<ENUM_SERVICE_STATUSW*>(buf.data());
                if (EnumDependentServicesW(audio, SERVICE_ACTIVE, list, bytes, &bytes, &count))
                    for (DWORD i = 0; i < count; ++i)  // in the order they have to be stopped
                    {
                        if (StopService(scm, list[i].lpServiceName)) Log(L"stopped %s", list[i].lpServiceName);
                        dependents.push_back(list[i].lpServiceName);
                    }
            }
            CloseServiceHandle(audio);
        }
        Log(L"Windows Audio %s", StopService(scm, L"Audiosrv") ? L"stopped" : L"did not stop");
        CloseServiceHandle(scm);
        return dependents;
    }

    void StartAudio(const std::vector<std::wstring>& dependents)
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) return;
        std::vector<std::wstring> names = {L"AudioEndpointBuilder", L"Audiosrv"};
        names.insert(names.end(), dependents.rbegin(), dependents.rend());  // reverse of the stop order
        for (const auto& name : names)
        {
            if (SC_HANDLE svc = OpenServiceW(scm, name.c_str(), SERVICE_START | SERVICE_QUERY_STATUS))
            {
                if (!StartServiceW(svc, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
                    Log(L"could not start %s (error %lu)", name.c_str(), GetLastError());
                else
                    WaitState(svc, SERVICE_RUNNING);
                CloseServiceHandle(svc);
            }
        }
        CloseServiceHandle(scm);
        Log(L"Windows Audio started");
    }

    // ------------------------------------------------------------------ certificate trust

    PCCERT_CONTEXT LoadCert(const std::wstring& path)
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return nullptr;
        std::vector<BYTE> bytes(64 * 1024);
        DWORD n = 0;
        const BOOL ok = ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr);
        CloseHandle(f);
        return ok ? CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, bytes.data(), n) : nullptr;
    }

    HCERTSTORE OpenMachineStore(const wchar_t* name)
    {
        return CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, name);
    }

    bool InStore(HCERTSTORE store, PCCERT_CONTEXT cert)
    {
        PCCERT_CONTEXT found = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                                          CERT_FIND_EXISTING, cert, nullptr);
        if (found) CertFreeCertificateContext(found);
        return found != nullptr;
    }

    void SetStateFlag(const wchar_t* name, DWORD value)
    {
        HKEY k = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSetupStateKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr,
                            &k, nullptr) == ERROR_SUCCESS)
        {
            RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
            RegCloseKey(k);
        }
    }

    DWORD StateFlag(const wchar_t* name)
    {
        DWORD v = 0, size = sizeof(v);
        RegGetValueW(HKEY_LOCAL_MACHINE, kSetupStateKey, name, RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &v, &size);
        return v;
    }

    // The publisher certificate goes to TrustedPublisher, so Windows installs the packages without asking (that
    // question cannot be shown from the installer service anyway). A self-made certificate (issuer == subject) is
    // also its own root; a bought one chains to a root Windows already trusts.
    bool TrustCertificate(const std::wstring& driverDir)
    {
        PCCERT_CONTEXT cert = LoadCert(driverDir + kCertFile);
        if (!cert)
        {
            Log(L"no %s in the package: certificate trust left as it is", kCertFile);
            return true;
        }
        const bool selfSigned = CertCompareCertificateName(X509_ASN_ENCODING, &cert->pCertInfo->Issuer, &cert->pCertInfo->Subject);
        bool ok = true;
        for (const wchar_t* storeName : {L"Root", L"TrustedPublisher"})
        {
            if (!selfSigned && wcscmp(storeName, L"Root") == 0) continue;
            HCERTSTORE store = OpenMachineStore(storeName);
            if (!store)
            {
                Log(L"could not open the %s store (error %lu)", storeName, GetLastError());
                ok = false;
                continue;
            }
            if (InStore(store, cert))
                Log(L"certificate already in %s", storeName);
            else if (CertAddCertificateContextToStore(store, cert, CERT_STORE_ADD_NEW, nullptr))
            {
                SetStateFlag(storeName, 1);
                Log(L"certificate added to %s", storeName);
            }
            else
            {
                Log(L"could not add the certificate to %s (error %lu)", storeName, GetLastError());
                ok = false;
            }
            CertCloseStore(store, 0);
        }
        CertFreeCertificateContext(cert);
        return ok;
    }

    void UntrustCertificate(const std::wstring& driverDir)
    {
        PCCERT_CONTEXT cert = LoadCert(driverDir + kCertFile);
        if (cert)
        {
            for (const wchar_t* storeName : {L"TrustedPublisher", L"Root"})
            {
                if (!StateFlag(storeName)) continue;  // it was there before us: leave it
                if (HCERTSTORE store = OpenMachineStore(storeName))
                {
                    PCCERT_CONTEXT found = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                                                      CERT_FIND_EXISTING, cert, nullptr);
                    if (found && CertDeleteCertificateFromStore(found))  // frees `found`
                        Log(L"certificate removed from %s", storeName);
                    CertCloseStore(store, 0);
                }
            }
            CertFreeCertificateContext(cert);
        }
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kSetupStateKey, KEY_WOW64_64KEY, 0);
    }

    // ------------------------------------------------------------------ the effect's log folders

    bool IsAdminOrSystem(PSID sid)
    {
        return sid && (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid));
    }

    // Removes one of our log folders, as SYSTEM, without ever acting through a link: a junction or symlink
    // is removed itself (never what it points to), files are deleted by name (a hard link loses only that
    // name), and subfolders that are not links are left alone. A folder whose owner is not SYSTEM,
    // Administrators or `allowedOwner` is not touched at all: its owner could swap it for a junction while
    // we delete inside it.
    void RemoveLogFolder(const std::wstring& dir, WELL_KNOWN_SID_TYPE allowedOwner)
    {
        const DWORD attrs = GetFileAttributesW(dir.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
            return;
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
        {
            Log(L"old log folder %s is a link: removed the link (%s)", dir.c_str(), RemoveDirectoryW(dir.c_str()) ? L"ok" : L"failed");
            return;
        }
        PSID owner = nullptr;
        PSECURITY_DESCRIPTOR sd = nullptr;
        const bool trusted = GetNamedSecurityInfoW(dir.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr,
                                                   nullptr, nullptr, &sd) == ERROR_SUCCESS &&
                             (IsAdminOrSystem(owner) || IsWellKnownSid(owner, allowedOwner));
        LocalFree(sd);
        if (!trusted)
        {
            Log(L"log folder %s has an untrusted owner: left alone", dir.c_str());
            return;
        }
        WIN32_FIND_DATAW fd{};
        HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                    continue;
                const std::wstring path = dir + L"\\" + fd.cFileName;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    DeleteFileW(path.c_str());
                else if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    RemoveDirectoryW(path.c_str());
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        Log(L"log folder %s removed (%s)", dir.c_str(), RemoveDirectoryW(dir.c_str()) ? L"ok" : L"not empty or in use");
    }

    // Before 2026-11-13 the effect logged to C:\ProgramData\WebrtcVoiceApo; it now logs to its own temp folder.
    void RemoveOldLogFolder()
    {
        const std::wstring programData = KnownFolder(FOLDERID_ProgramData);
        if (!programData.empty())
            RemoveLogFolder(programData + L"\\WebrtcVoiceApo", WinLocalSystemSid);
    }

    // The effect's current log folder: audiodg's (LOCAL SERVICE) temp folder, see src\Log.cpp. Only SYSTEM,
    // Administrators and LOCAL SERVICE can open that temp folder, and the effect itself owns the subfolder.
    void RemoveEffectLogFolder()
    {
        RemoveLogFolder(WindowsDir() + L"\\ServiceProfiles\\LocalService\\AppData\\Local\\Temp\\WebrtcAudioEngine",
                        WinLocalServiceSid);
    }

    // ------------------------------------------------------------------ install / uninstall

    UINT DoInstall()
    {
        const std::wstring driverDir = CustomActionData();
        if (driverDir.empty()) return ERROR_INSTALL_FAILURE;
        Log(L"installing from %s", driverDir.c_str());

        TrustCertificate(driverDir);  // a failure shows up as a refused package below
        RemoveOldLogFolder();

        const std::vector<OemInf> before = OurPackages();
        UINT result = ERROR_SUCCESS;
        const std::vector<std::wstring> dependents = StopAudio();
        for (size_t i = 0; i < std::size(kInfs); ++i)
        {
            const std::wstring inf = driverDir + kInfDirs[i] + L"\\" + kInfs[i];
            BOOL reboot = FALSE;
            // Components first, so they are in the store when the extension creates their software devices.
            // ERROR_NO_MORE_ITEMS: the package is in the store, but a device already has an equal or better one.
            if (DiInstallDriverW(nullptr, inf.c_str(), 0, &reboot) || GetLastError() == ERROR_NO_MORE_ITEMS)
                Log(L"installed %s%s", kInfs[i], reboot ? L" (restart needed)" : L"");
            else
            {
                const DWORD e = GetLastError();
                Log(L"FAILED to install %s (error 0x%08lX)", inf.c_str(), e);
                result = ERROR_INSTALL_FAILURE;
                break;
            }
        }
        StartAudio(dependents);
        if (result != ERROR_SUCCESS) return result;

        // Remove superseded versions (e.g. from an older script install). No force: a package still in use stays.
        const std::vector<OemInf> after = OurPackages();
        for (const wchar_t* inf : kInfs)
        {
            bool added = false;
            for (const auto& p : after)
                added = added || (p.original == inf && !Contains(before, p.published));
            if (!added) continue;
            for (const auto& old : before)
            {
                if (old.original != inf) continue;
                const BOOL ok = SetupUninstallOEMInfW(old.published.c_str(), 0, nullptr);
                Log(L"old package %s (%s): %s", old.published.c_str(), inf, ok ? L"removed" : L"kept (in use)");
            }
        }
        // The settings app is installed by Windows from the app package (AddSoftware), a few seconds later.
        return ERROR_SUCCESS;
    }

    void RemoveOurDevices()
    {
        HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
        if (set == INVALID_HANDLE_VALUE) return;
        SP_DEVINFO_DATA dev{};
        dev.cbSize = sizeof(dev);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i)
        {
            wchar_t ids[2048] = {};
            if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(ids),
                                                   sizeof(ids) - 2 * sizeof(wchar_t), nullptr))
                continue;
            bool ours = false;
            for (const wchar_t* id = ids; *id; id += wcslen(id) + 1)
                for (const wchar_t* hw : kHardwareIds)
                    ours = ours || _wcsicmp(id, hw) == 0;
            if (!ours) continue;
            BOOL reboot = FALSE;
            Log(L"device %s", DiUninstallDevice(nullptr, set, &dev, 0, &reboot) ? L"removed" : L"could not be removed");
        }
        SetupDiDestroyDeviceInfoList(set);
    }

    void RemoveSettingsApp()
    {
        // Program Files copy, Start-menu shortcut, sign-in start, Device Setup Manager record: the app knows its parts.
        const std::wstring exe = KnownFolder(FOLDERID_ProgramFiles) + L"\\WebrtcAudioEngine\\WebrtcAudioEngineSettings.exe";
        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            Log(L"settings app not installed");
            return;
        }
        std::wstring cmd = L"\"" + exe + L"\" /uninstall";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            Log(L"could not run the settings app's uninstall (error %lu)", GetLastError());
            return;
        }
        DWORD code = 1;
        if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Log(L"settings app uninstall: code %lu (log: %%ProgramData%%\\WebrtcAudioEngine\\setup.log)", code);
    }

    UINT DoUninstall()
    {
        const std::wstring driverDir = CustomActionData();
        const std::wstring infDir = WindowsDir() + L"\\INF\\";
        std::vector<OemInf> packages = OurPackages();
        const std::vector<std::wstring> dependents = StopAudio();
        // Extension first (it stops re-creating the software devices), then the components.
        for (const wchar_t* inf : {kInfs[2], kInfs[0], kInfs[1]})
        {
            for (const auto& p : packages)
            {
                if (p.original != inf) continue;
                BOOL reboot = FALSE;
                const std::wstring path = infDir + p.published;
                if (DiUninstallDriverW(nullptr, path.c_str(), 0, &reboot))
                    Log(L"uninstalled %s (%s)", p.published.c_str(), inf);
                else
                    Log(L"could not uninstall %s (%s), error 0x%08lX", p.published.c_str(), inf, GetLastError());
            }
        }
        RemoveOurDevices();
        StartAudio(dependents);
        RemoveSettingsApp();
        if (!driverDir.empty()) UntrustCertificate(driverDir);
        return ERROR_SUCCESS;  // best effort: never block removing the product
    }

    template <typename F>
    UINT Run(MSIHANDLE session, F&& f)
    {
        g_session = session;
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        UINT r = ERROR_INSTALL_FAILURE;
        try { r = f(); }
        catch (...) { Log(L"unexpected exception"); }
        if (SUCCEEDED(co)) CoUninitialize();
        return r;
    }
}

extern "C" __declspec(dllexport) UINT __stdcall InstallDrivers(MSIHANDLE session)
{
    return Run(session, DoInstall);
}

extern "C" __declspec(dllexport) UINT __stdcall UninstallDrivers(MSIHANDLE session)
{
    return Run(session, DoUninstall);
}

extern "C" __declspec(dllexport) UINT __stdcall RemoveLogs(MSIHANDLE session)
{
    return Run(session, [] {
        RemoveOldLogFolder();
        RemoveEffectLogFolder();
        return static_cast<UINT>(ERROR_SUCCESS);  // best effort
    });
}
