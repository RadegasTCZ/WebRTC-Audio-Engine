#include "Log.h"
#include <objbase.h>
#include <aclapi.h>
#include <sddl.h>
#include <strsafe.h>
#include <algorithm>
#include <functional>
#include <cstdarg>
#include <string>
#include <vector>

// Segmented log in the writing account's temp folder: %TEMP%\WebrtcAudioEngine\engine-<local time>.log.
// For the effect inside audiodg (LOCAL SERVICE) that is
// C:\Windows\ServiceProfiles\LocalService\AppData\Local\Temp\WebrtcAudioEngine, which only SYSTEM,
// Administrators and LOCAL SERVICE can open: ordinary users cannot plant anything there.
//
// A new segment starts with each process (each audio session), at local midnight and at 1 MB. Each new
// segment deletes our own old ones: only the newest 10 are kept, and none older than 14 days.

namespace
{
    constexpr wchar_t kFolderName[] = L"WebrtcAudioEngine";
    constexpr LONGLONG kSegmentBytes = 1024 * 1024;
    constexpr size_t kKeepSegments = 10;
    constexpr size_t kKeepDumps = 3;  // diagnostic recordings (aecdump-*.wav, up to 46 MB each)
    constexpr ULONGLONG kKeepAge = 14ull * 24 * 3600 * 10'000'000;  // 14 days in 100 ns units
    // Folder ACL when we create it: SYSTEM, Administrators, LOCAL SERVICE (audiodg) and the folder's owner
    // (the account that created it). Protected: nothing is inherited from the temp folder above.
    constexpr wchar_t kFolderSddl[] = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;LS)(A;OICI;FA;;;OW)";

    SRWLOCK g_lock = SRWLOCK_INIT;  // guards everything below
    int g_dirState = -1;            // -1 = not prepared yet, 0 = no file log in this process, 1 = ok
    wchar_t g_dir[MAX_PATH] = L"";
    wchar_t g_file[MAX_PATH] = L"";  // current segment; empty = start one with the next line
    DWORD g_fileDate = 0;            // yyyymmdd of the current segment
    LONGLONG g_fileBytes = 0;

    // The folder must be one only trusted accounts control: owned by SYSTEM, Administrators or the account
    // this process runs as. A folder someone else made in advance (e.g. where %TEMP% falls back to the
    // shared C:\Windows\Temp) could redirect our writes and deletes, so then there is no file log.
    bool OwnerTrusted(PSID owner) noexcept
    {
        if (!owner)
            return false;
        if (IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid))
            return true;
        bool self = false;
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        {
            BYTE buf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
            DWORD len = 0;
            if (GetTokenInformation(token, TokenUser, buf, sizeof(buf), &len))
                self = EqualSid(owner, reinterpret_cast<TOKEN_USER*>(buf)->User.Sid) != FALSE;
            CloseHandle(token);
        }
        return self;
    }

    bool PrepareDir() noexcept
    {
        wchar_t temp[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, temp);
        if (n == 0 || n >= MAX_PATH || FAILED(StringCchPrintfW(g_dir, MAX_PATH, L"%s%s", temp, kFolderName)))
            return false;
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(kFolderSddl, SDDL_REVISION_1, &sd, nullptr))
        {
            SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
            CreateDirectoryW(g_dir, &sa);  // fails harmlessly if it already exists; checked below either way
            LocalFree(sd);
        }
        const DWORD attrs = GetFileAttributesW(g_dir);
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
        PSID owner = nullptr;
        PSECURITY_DESCRIPTOR osd = nullptr;
        bool ok = GetNamedSecurityInfoW(g_dir, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &osd) ==
                      ERROR_SUCCESS &&
                  OwnerTrusted(owner);
        LocalFree(osd);
        if (!ok)
            OutputDebugStringW(L"WebrtcVoiceApo: log folder is a link or not owned by a trusted account: no file log\r\n");
        return ok;
    }

    // Deletes our own old segments: all but the newest kKeepSegments, and any older than kKeepAge. Never the
    // current one; never anything that is not a plain engine-*.log file.
    void Cleanup() noexcept
    {
        try
        {
            struct Seg { std::wstring name; ULONGLONG written; };
            std::vector<Seg> segs;
            WIN32_FIND_DATAW fd{};
            HANDLE f = FindFirstFileExW((std::wstring(g_dir) + L"\\engine-*.log").c_str(), FindExInfoBasic, &fd,
                                        FindExSearchNameMatch, nullptr, 0);
            if (f == INVALID_HANDLE_VALUE)
                return;
            do
            {
                if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
                    continue;
                segs.push_back({fd.cFileName, (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                                                  fd.ftLastWriteTime.dwLowDateTime});
            } while (FindNextFileW(f, &fd));
            FindClose(f);

            // The names hold the start time (yyyy-mm-dd_hhmmss), so name order is age order: newest first.
            std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) { return a.name > b.name; });
            FILETIME nowFt;
            GetSystemTimeAsFileTime(&nowFt);
            const ULONGLONG now = (static_cast<ULONGLONG>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;
            const wchar_t* current = wcsrchr(g_file, L'\\');
            current = current ? current + 1 : g_file;
            size_t kept = g_file[0] ? 1 : 0;  // the current segment counts, even before its first line is on disk
            for (const Seg& seg : segs)
            {
                if (_wcsicmp(seg.name.c_str(), current) == 0)
                    continue;
                if (kept < kKeepSegments && !(now > seg.written && now - seg.written > kKeepAge))
                    ++kept;
                else
                    DeleteFileW((std::wstring(g_dir) + L"\\" + seg.name).c_str());
            }
        }
        catch (...)
        {
            // out of memory: clean up next time
        }
    }

    // Keeps the newest kKeepDumps diagnostic recordings (names hold the time, so name order is age order).
    void CleanupDumps() noexcept
    {
        try
        {
            std::vector<std::wstring> names;
            WIN32_FIND_DATAW fd{};
            HANDLE f = FindFirstFileExW((std::wstring(g_dir) + L"\\aecdump-*.wav").c_str(), FindExInfoBasic, &fd,
                                        FindExSearchNameMatch, nullptr, 0);
            if (f == INVALID_HANDLE_VALUE)
                return;
            do
                if (!(fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
                    names.push_back(fd.cFileName);
            while (FindNextFileW(f, &fd));
            FindClose(f);
            std::sort(names.begin(), names.end(), std::greater<>());
            for (size_t i = kKeepDumps; i < names.size(); ++i)
                DeleteFileW((std::wstring(g_dir) + L"\\" + names[i]).c_str());
        }
        catch (...)
        {
        }
    }

    void StartSegment(const SYSTEMTIME& t, DWORD date) noexcept
    {
        g_file[0] = L'\0';
        for (int k = 1; k <= 9; ++k)  // two segments in the same second: _2, _3, ...
        {
            wchar_t path[MAX_PATH];
            const HRESULT hr = k == 1 ? StringCchPrintfW(path, MAX_PATH, L"%s\\engine-%04u-%02u-%02u_%02u%02u%02u.log", g_dir, t.wYear,
                                                         t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond)
                                      : StringCchPrintfW(path, MAX_PATH, L"%s\\engine-%04u-%02u-%02u_%02u%02u%02u_%d.log", g_dir,
                                                         t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, k);
            if (FAILED(hr))
                return;
            if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
            {
                wcscpy_s(g_file, path);
                break;
            }
        }
        g_fileDate = date;
        g_fileBytes = 0;
        Cleanup();
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

        AcquireSRWLockExclusive(&g_lock);
        if (g_dirState < 0)
            g_dirState = PrepareDir() ? 1 : 0;
        if (g_dirState == 1)
        {
            const DWORD date = t.wYear * 10000u + t.wMonth * 100u + t.wDay;
            if (!g_file[0] || date != g_fileDate || g_fileBytes >= kSegmentBytes)
                StartSegment(t, date);
            // FILE_FLAG_OPEN_REPARSE_POINT: never follow a link, even inside our own folder.
            HANDLE h = g_file[0] ? CreateFileW(g_file, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)
                                 : INVALID_HANDLE_VALUE;
            if (h != INVALID_HANDLE_VALUE)
            {
                FILE_ATTRIBUTE_TAG_INFO tag{};
                LARGE_INTEGER size{};
                if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag)) &&
                    !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && GetFileSizeEx(h, &size))
                {
                    DWORD written = 0;
                    WriteFile(h, utf8, static_cast<DWORD>(n - 1), &written, nullptr);
                    g_fileBytes = size.QuadPart + written;  // from the file itself: also right if a cleaner removed it
                }
                CloseHandle(h);
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
    }

    bool WriteDiagFile(const wchar_t* name, const void* head, size_t headBytes, const void* data, size_t bytes,
                       wchar_t (&pathOut)[MAX_PATH]) noexcept
    {
        pathOut[0] = L'\0';
        if (!name || wcschr(name, L'\\') || wcschr(name, L'/'))
            return false;
        AcquireSRWLockExclusive(&g_lock);
        if (g_dirState < 0)
            g_dirState = PrepareDir() ? 1 : 0;
        bool ok = false;
        if (g_dirState == 1 && SUCCEEDED(StringCchPrintfW(pathOut, MAX_PATH, L"%s\\%s", g_dir, name)))
        {
            // CREATE_NEW + never follow a link (same rules as the log).
            HANDLE h = CreateFileW(pathOut, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                auto put = [h](const void* from, size_t n) {
                    const BYTE* p = static_cast<const BYTE*>(from);
                    bool good = true;
                    while (good && n > 0)
                    {
                        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(n, 1u << 24));
                        DWORD written = 0;
                        good = WriteFile(h, p, chunk, &written, nullptr) && written == chunk;
                        p += chunk;
                        n -= chunk;
                    }
                    return good;
                };
                ok = put(head, headBytes) && put(data, bytes);
                CloseHandle(h);
                if (!ok)
                    DeleteFileW(pathOut);
            }
            CleanupDumps();
        }
        ReleaseSRWLockExclusive(&g_lock);
        return ok;
    }
}
