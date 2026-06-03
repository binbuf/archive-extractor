#include "archive_core/logging.h"

#include <cstdio>
#include <mutex>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>   // SHGetKnownFolderPath
#include <knownfolders.h>

namespace ae {

namespace {

// All file-log state is guarded by one mutex so Log() is safe to call from the
// worker thread and the UI thread concurrently.
std::mutex g_logMutex;
std::wstring g_logPath;          // resolved log.txt path ("" => disabled)
std::size_t g_maxBytes = 0;      // rotation threshold
bool g_inited = false;

// Resolve %LOCALAPPDATA%\ArchiveExtractor and ensure the directory exists.
// Returns the directory path, or empty on failure.
std::wstring EnsureAppDataDir() {
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) ||
        !base) {
        if (base) CoTaskMemFree(base);
        return std::wstring();
    }
    std::wstring dir(base);
    CoTaskMemFree(base);
    if (!dir.empty() && dir.back() != L'\\') dir.push_back(L'\\');
    dir += L"ArchiveExtractor";
    // CreateDirectory is fine even if it already exists.
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::size_t FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        return 0;
    }
    return (static_cast<std::size_t>(fad.nFileSizeHigh) << 32) |
           fad.nFileSizeLow;
}

// Rotate log.txt -> log.txt.1 (replacing any prior .1) when it has grown past
// the cap. Best-effort; failures are silently ignored (logging must never throw
// or block the app). Caller holds g_logMutex.
void RotateIfNeededLocked(std::size_t incomingBytes) {
    if (g_logPath.empty() || g_maxBytes == 0) return;
    const std::size_t cur = FileSize(g_logPath);
    if (cur + incomingBytes <= g_maxBytes) return;
    const std::wstring backup = g_logPath + L".1";
    DeleteFileW(backup.c_str());
    MoveFileExW(g_logPath.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Append `line` (already newline-terminated) to the log file as UTF-8. Caller
// holds g_logMutex.
void AppendToFileLocked(const std::wstring& line) {
    if (g_logPath.empty()) return;

    const int needed = WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
                                           static_cast<int>(line.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return;
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
                        static_cast<int>(line.size()), utf8.data(), needed,
                        nullptr, nullptr);

    RotateIfNeededLocked(utf8.size());

    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
              nullptr);
    CloseHandle(h);
}

}  // namespace

bool InitFileLog(std::wstring_view path, std::size_t maxBytes) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_maxBytes = maxBytes;
    if (!path.empty()) {
        g_logPath.assign(path);
    } else {
        const std::wstring dir = EnsureAppDataDir();
        if (dir.empty()) {
            g_logPath.clear();
            g_inited = false;
            return false;
        }
        g_logPath = dir + L"\\log.txt";
    }
    g_inited = true;
    return true;
}

std::wstring LogFilePath() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_logPath;
}

void ShutdownFileLog() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logPath.clear();
    g_inited = false;
}

void Log(std::wstring_view message) {
    std::wstring line(message);
    line.push_back(L'\n');
    OutputDebugStringW(line.c_str());
    // Best-effort console echo; silently fails if no console is attached.
    std::fputws(line.c_str(), stderr);

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_inited) AppendToFileLocked(line);
}

}  // namespace ae
