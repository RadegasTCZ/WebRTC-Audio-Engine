#include "Log.h"
#include <objbase.h>
#include <aclapi.h>
#include <strsafe.h>
#include <atomic>
#include <cstdarg>

namespace
{
    SRWLOCK g_lock = SRWLOCK_INIT;
    constexpr wchar_t kLogPath[] = L"C:\\ProgramData\\WebrtcVoiceApo\\engine.log";
    constexpr wchar_t kOldLogPath[] = L"C:\\ProgramData\\WebrtcVoiceApo\\engine.old.log";
    constexpr wchar_t kLogDir[] = L"C:\\ProgramData\\WebrtcVoiceApo";
    constexpr LONGLONG kMaxLogBytes = 2 * 1024 * 1024;  // then rotate: at most ~4 MB on disk

    // Write the log only into a folder the installer made: a real folder (not a junction) owned by SYSTEM or
    // Administrators. Any user may create folders in ProgramData, so a folder made in advance by someone else
    // could redirect our writes and the rotation's move elsewhere. Checked once per process: such a trusted
    // folder cannot be replaced afterwards by an ordinary user.
    std::atomic<int> g_dirTrusted{-1};  // -1 = not checked yet, 0 = no file log, 1 = ok
    bool LogDirTrusted() noexcept
    {
        int t = g_dirTrusted.load(std::memory_order_acquire);
        if (t >= 0)
            return t == 1;
        bool ok = false;
        const DWORD attrs = GetFileAttributesW(kLogDir);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT))
        {
            PSID owner = nullptr;
            PSECURITY_DESCRIPTOR sd = nullptr;
            if (GetNamedSecurityInfoW(kLogDir, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) ==
                    ERROR_SUCCESS &&
                owner)
                ok = IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid);
            LocalFree(sd);
        }
        if (attrs == INVALID_FILE_ATTRIBUTES)
            return false;  // not there (yet): decide when it exists
        g_dirTrusted.store(ok ? 1 : 0, std::memory_order_release);
        if (!ok)
            OutputDebugStringW(L"WebrtcVoiceApo: log folder is not owned by SYSTEM/Administrators or is a link: no file log\r\n");
        return ok;
    }
}

namespace apolog
{
    void GuidToString(REFGUID g, wchar_t (&out)[40]) noexcept
    {
        if (StringFromGUID2(g, out, 40) == 0)
            out[0] = L'\0';
    }

    void Write(const wchar_t* fmt, ...) noexcept
    {
        wchar_t msg[1024];
        va_list args;
        va_start(args, fmt);
        StringCchVPrintfW(msg, ARRAYSIZE(msg), fmt, args);
        va_end(args);

        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t line[1200];
        StringCchPrintfW(line, ARRAYSIZE(line), L"%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu tid=%lu  %s\r\n",
                         t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                         GetCurrentProcessId(), GetCurrentThreadId(), msg);

        OutputDebugStringW(line);  // visible in DebugView even if the file can't be opened

        // Store as UTF-8.
        char utf8[2400];
        int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
        if (n <= 1)
            return;

        if (!LogDirTrusted())
            return;
        AcquireSRWLockExclusive(&g_lock);
        // FILE_FLAG_OPEN_REPARSE_POINT: never follow a symlink/junction planted in place of the log.
        HANDLE h = CreateFileW(kLogPath, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            FILE_ATTRIBUTE_TAG_INFO tag{};
            LARGE_INTEGER size{};
            const bool plainFile = GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag)) &&
                                   !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
            if (plainFile && GetFileSizeEx(h, &size) && size.QuadPart > kMaxLogBytes)
            {
                // Keep one previous log (engine.old.log) and start over.
                CloseHandle(h);
                MoveFileExW(kLogPath, kOldLogPath, MOVEFILE_REPLACE_EXISTING);
                h = CreateFileW(kLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            }
            if (h != INVALID_HANDLE_VALUE)
            {
                if (plainFile)
                {
                    DWORD written = 0;
                    WriteFile(h, utf8, static_cast<DWORD>(n - 1), &written, nullptr);
                }
                CloseHandle(h);
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
}
