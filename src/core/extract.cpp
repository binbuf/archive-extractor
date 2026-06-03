#include "archive_core/extract.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <objbase.h>  // CoCreateGuid

#include <archive.h>
#include <archive_entry.h>

#include "archive_core/logging.h"

namespace ae {

// Baseline inner-name sanitization hook (defined below). Reserved-name /
// invalid-char / trailing-dot handling is completed in task 12; for now this is
// a near pass-through so the hook exists at the right place in the flow.
std::wstring SanitizeInnerName(std::wstring_view name);

namespace {

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

bool IsSeparator(wchar_t c) { return c == L'\\' || c == L'/'; }

// Normalize separators to backslash and collapse mixed slashes.
std::wstring NormalizeSeparators(std::wstring_view s) {
    std::wstring out(s);
    std::replace(out.begin(), out.end(), L'/', L'\\');
    return out;
}

// Append `child` under `base` with a single separator.
std::wstring JoinPath(std::wstring_view base, std::wstring_view child) {
    std::wstring out(base);
    if (!out.empty() && !IsSeparator(out.back())) out.push_back(L'\\');
    out.append(child);
    return out;
}

// The volume root prefix of a path: "C:\" for drive paths, "\\server\share\"
// for UNC. Empty if it cannot be determined.
std::wstring VolumeRoot(std::wstring_view path) {
    std::wstring p = NormalizeSeparators(path);
    // Strip an extended prefix for analysis.
    if (p.rfind(L"\\\\?\\", 0) == 0) p = p.substr(4);
    if (p.rfind(L"UNC\\", 0) == 0) p = L"\\\\" + p.substr(4);

    if (p.size() >= 2 && p[1] == L':') {
        return p.substr(0, 2) + L"\\";
    }
    if (p.rfind(L"\\\\", 0) == 0) {
        // \\server\share\...
        std::size_t i = 2;
        int parts = 0;
        std::size_t end = p.size();
        for (; i < p.size(); ++i) {
            if (p[i] == L'\\') {
                ++parts;
                if (parts == 2) {
                    end = i;
                    break;
                }
            }
        }
        return p.substr(0, end) + L"\\";
    }
    return {};
}

}  // namespace

std::wstring ToExtendedPath(std::wstring_view path) {
    std::wstring p = NormalizeSeparators(path);
    if (p.empty()) return p;
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;  // already extended
    if (p.rfind(L"\\\\", 0) == 0) {
        // UNC: \\server\share -> \\?\UNC\server\share
        return L"\\\\?\\UNC\\" + p.substr(2);
    }
    if (p.size() >= 2 && p[1] == L':') {
        return L"\\\\?\\" + p;
    }
    return p;  // relative / unrecognized: leave as-is
}

namespace {

// ---------------------------------------------------------------------------
// Directory / file creation (extended-path, recursive)
// ---------------------------------------------------------------------------

// Create every directory along `dir` (a normal absolute path). Returns true if
// the directory exists afterward.
bool CreateDirsRecursive(std::wstring_view dir) {
    std::wstring norm = NormalizeSeparators(dir);
    if (norm.empty()) return false;
    const std::wstring root = VolumeRoot(norm);

    // Walk segment by segment, creating each.
    std::wstring accum;
    std::size_t start = 0;
    if (!root.empty() && norm.rfind(root.substr(0, root.size() - 1), 0) == 0) {
        accum = root;                  // begin past the volume root
        accum.pop_back();              // drop trailing slash; rebuilt below
        start = root.size();           // skip "C:\"
    }
    for (std::size_t i = start; i <= norm.size(); ++i) {
        if (i == norm.size() || norm[i] == L'\\') {
            if (i > start) {
                if (!accum.empty() && accum.back() != L'\\') accum.push_back(L'\\');
                accum.append(norm.substr(start, i - start));
                const std::wstring ext = ToExtendedPath(accum);
                if (!CreateDirectoryW(ext.c_str(), nullptr)) {
                    const DWORD e = GetLastError();
                    if (e != ERROR_ALREADY_EXISTS) {
                        // Could be the volume root itself; tolerate.
                        const DWORD attr = GetFileAttributesW(ext.c_str());
                        if (attr == INVALID_FILE_ATTRIBUTES ||
                            !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                            return false;
                        }
                    }
                }
            }
            start = i + 1;
        }
    }
    const DWORD attr = GetFileAttributesW(ToExtendedPath(norm).c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// ---------------------------------------------------------------------------
// Timestamp conversion (Unix time_t -> Windows FILETIME), clamped to the FS
// range. NTFS epoch is 1601; the design clamps DOS-range issues (pre-1980,
// post-2107) but the real constraint is the FILETIME range. Clamp generously.
// ---------------------------------------------------------------------------

FILETIME UnixToFileTime(time_t unixSeconds) {
    // Clamp to [1980-01-01, 2107-12-31] per design §12. These are seconds since
    // the Unix epoch.
    constexpr long long kMin = 315532800LL;    // 1980-01-01T00:00:00Z
    constexpr long long kMax = 4354819199LL;   // 2107-12-31T23:59:59Z
    long long s = static_cast<long long>(unixSeconds);
    if (s < kMin) s = kMin;
    if (s > kMax) s = kMax;
    // FILETIME = 100ns intervals since 1601-01-01. Unix epoch is 11644473600s
    // after that.
    const unsigned long long ft =
        (static_cast<unsigned long long>(s) + 11644473600ULL) * 10000000ULL;
    FILETIME out;
    out.dwLowDateTime = static_cast<DWORD>(ft & 0xFFFFFFFFULL);
    out.dwHighDateTime = static_cast<DWORD>(ft >> 32);
    return out;
}

// ---------------------------------------------------------------------------
// Path-safety (zip-slip). Returns true if `entryPath` is a safe RELATIVE path
// that stays inside the staging root after normalization. On success, writes
// the cleaned relative path (backslash-separated, no leading/trailing sep).
// ---------------------------------------------------------------------------

bool SanitizeEntryPath(std::wstring_view entryPath, std::wstring& outRelative) {
    std::wstring p = NormalizeSeparators(entryPath);

    // Reject absolute paths: drive-letter (C:\), UNC (\\server), leading sep.
    if (p.empty()) return false;
    if (IsSeparator(p.front())) return false;       // leading "/" or "\"
    if (p.size() >= 2 && p[1] == L':') return false;  // "C:..."
    if (p.rfind(L"\\\\", 0) == 0) return false;       // UNC

    // Split into segments, resolving "." and "..". Any ".." that would pop
    // above the root is a traversal escape -> reject the whole archive.
    std::vector<std::wstring> stack;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= p.size(); ++i) {
        if (i == p.size() || p[i] == L'\\') {
            std::wstring seg = p.substr(start, i - start);
            start = i + 1;
            if (seg.empty() || seg == L".") continue;
            if (seg == L"..") {
                if (stack.empty()) return false;  // escapes the root
                stack.pop_back();
                continue;
            }
            // Inner-name sanitization hook (reserved names / invalid chars):
            // baseline pass-through; full handling is task 12.
            stack.push_back(SanitizeInnerName(seg));
        }
    }
    if (stack.empty()) return false;

    std::wstring rel;
    for (std::size_t i = 0; i < stack.size(); ++i) {
        if (i) rel.push_back(L'\\');
        rel.append(stack[i]);
    }
    outRelative = std::move(rel);
    return true;
}

// ---------------------------------------------------------------------------
// libarchive RAII
// ---------------------------------------------------------------------------

struct ArchiveReader {
    struct archive* a = nullptr;
    ~ArchiveReader() {
        if (a) archive_read_free(a);
    }
};

const char* ErrText(struct archive* a) {
    const char* s = archive_error_string(a);
    return s ? s : "unknown libarchive error";
}

std::wstring Widen(const char* s) {
    if (!s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    return out;
}

// Throttled progress emitter.
class ProgressThrottle {
   public:
    explicit ProgressThrottle(const ExtractCallbacks& cb) : cb_(cb) {
        last_ = std::chrono::steady_clock::now() -
                std::chrono::milliseconds(1000);
    }
    void Emit(std::uint64_t done, std::uint64_t total,
              std::wstring_view entry, bool force = false) {
        if (!cb_.onProgress) return;
        const auto now = std::chrono::steady_clock::now();
        if (!force &&
            now - last_ < std::chrono::milliseconds(40)) {
            return;
        }
        last_ = now;
        Progress p;
        p.done = done;
        p.total = total;
        p.currentEntry = std::wstring(entry);
        cb_.onProgress(p);
    }

   private:
    const ExtractCallbacks& cb_;
    std::chrono::steady_clock::time_point last_;
};

}  // namespace

namespace {

ExtractResult Fail(ExtractStatus st, std::wstring msg) {
    ExtractResult r;
    r.status = st;
    r.message = std::move(msg);
    return r;
}

}  // namespace

// Baseline inner-name sanitization. For task 03 this is a near pass-through
// hook; reserved-name / invalid-char / trailing-dot handling is completed in
// task 12. We do strip trailing dots/spaces on the LAST segment only at the
// caller layer; here we leave the name intact to keep the hook minimal.
std::wstring SanitizeInnerName(std::wstring_view name) {
    return std::wstring(name);
}

ExtractResult LibarchiveExtractor::extractToStaging(
    const PipelinePlan& plan, std::wstring_view sourcePath,
    std::wstring_view stagingDir, const ExtractCallbacks& callbacks) {
    if (plan.backend != Backend::LibArchive) {
        return Fail(ExtractStatus::Unsupported,
                    L"plan backend is not libarchive");
    }

    ArchiveReader reader;
    reader.a = archive_read_new();
    if (!reader.a) {
        return Fail(ExtractStatus::OpenFailed, L"archive_read_new failed");
    }
    struct archive* a = reader.a;

    // Single-stream inputs (bare .gz/.bz2/.xz/.zst/.lz4) carry no container; use
    // the "raw" format so libarchive yields exactly one nameless data entry.
    const bool singleStream = (plan.kind == Kind::SingleStream);
    archive_read_support_filter_all(a);
    if (singleStream) {
        archive_read_support_format_raw(a);
    } else {
        archive_read_support_format_all(a);
    }

    // Honor the zip UTF-8 flag with an OEM/ANSI fallback heuristic: ask
    // libarchive to interpret non-UTF-8 zip names via the active code page.
    // (libarchive reads bit 11 itself; hdrcharset steers the fallback.)
    archive_read_set_option(a, "zip", "hdrcharset", "UTF-8");

    // Password hook (task 10 supplies a real prompt; here it returns nullopt).
    if (callbacks.requestPassword) {
        if (auto pw = callbacks.requestPassword()) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, pw->c_str(), -1,
                                               nullptr, 0, nullptr, nullptr);
            if (n > 0) {
                std::string utf8(static_cast<std::size_t>(n - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, pw->c_str(), -1, utf8.data(), n,
                                    nullptr, nullptr);
                archive_read_add_passphrase(a, utf8.c_str());
            }
        }
    }

    const std::wstring src = ToExtendedPath(sourcePath);
    if (archive_read_open_filename_w(a, src.c_str(), 64 * 1024) != ARCHIVE_OK) {
        return Fail(ExtractStatus::OpenFailed,
                    L"cannot open archive: " + Widen(ErrText(a)));
    }

    // --- Progress sizing ----------------------------------------------------
    // Containers: total compressed size of the source as the denominator when
    // entry sizes are unknown; we also accumulate uncompressed bytes written.
    // Single-stream: drive by compressed bytes read vs file size.
    std::uint64_t sourceSize = 0;
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fad)) {
            sourceSize = (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) |
                         fad.nFileSizeLow;
        }
    }

    ProgressThrottle progress(callbacks);
    ExtractResult result;
    std::uint64_t bytesWritten = 0;

    const std::wstring stagingRoot = NormalizeSeparators(stagingDir);

    struct archive_entry* entry = nullptr;
    int r;
    std::uint64_t singleStreamIndex = 0;
    while ((r = archive_read_next_header(a, &entry)) != ARCHIVE_EOF) {
        if (callbacks.cancel && callbacks.cancel->IsCancelled()) {
            return Fail(ExtractStatus::Cancelled, L"cancelled");
        }
        if (r == ARCHIVE_RETRY) continue;
        if (r < ARCHIVE_WARN) {
            return Fail(ExtractStatus::CorruptInput,
                        L"read error: " + Widen(ErrText(a)));
        }
        // ARCHIVE_WARN is non-fatal; proceed.

        // --- Entry name ----------------------------------------------------
        std::wstring entryName;
        if (singleStream) {
            // The raw format yields one nameless entry. Use the planned output
            // name hint; fall back to the stem.
            entryName = !plan.outputNameHint.empty() ? plan.outputNameHint
                                                      : plan.stem;
            if (entryName.empty()) entryName = L"output";
            if (singleStreamIndex > 0) {
                // Defensive: a single-stream input must yield exactly one file.
                break;
            }
            ++singleStreamIndex;
        } else {
            const wchar_t* wname = archive_entry_pathname_w(entry);
            if (wname && *wname) {
                entryName = wname;
            } else {
                entryName = Widen(archive_entry_pathname(entry));
            }
            if (entryName.empty()) continue;  // nameless container entry: skip
        }

        // --- Entry type ----------------------------------------------------
        const auto ftype = archive_entry_filetype(entry);
        const bool isDir = (ftype == AE_IFDIR);
        const bool isReg = (ftype == AE_IFREG);
        const bool isLink = archive_entry_symlink_w(entry) != nullptr ||
                            archive_entry_symlink(entry) != nullptr ||
                            archive_entry_hardlink_w(entry) != nullptr ||
                            archive_entry_hardlink(entry) != nullptr;

        if (isLink) {
            // Detect + skip + log (full copy/skip prompt is task 12).
            result.hadLinks = true;
            ++result.entriesSkipped;
            Log(L"skipping link entry: " + entryName);
            continue;
        }
        if (!isDir && !isReg && !singleStream) {
            // Device/FIFO/socket/other special entry: skip + log, never fail.
            ++result.entriesSkipped;
            Log(L"skipping special entry: " + entryName);
            continue;
        }

        // --- Path safety (zip-slip) ----------------------------------------
        std::wstring rel;
        if (!singleStream) {
            if (!SanitizeEntryPath(entryName, rel)) {
                Log(L"rejected unsafe entry path: " + entryName);
                return Fail(ExtractStatus::Untrusted,
                            L"archive contains an unsafe path: " + entryName);
            }
        } else {
            // Single-stream: the output name is our own hint, but still pass it
            // through sanitization to strip any path separators defensively.
            if (!SanitizeEntryPath(entryName, rel)) {
                rel = L"output";
            }
        }

        const std::wstring fullPath = JoinPath(stagingRoot, rel);
        const std::wstring fullExt = ToExtendedPath(fullPath);

        if (isDir) {
            if (!CreateDirsRecursive(fullPath)) {
                return Fail(ExtractStatus::WriteFailed,
                            L"cannot create directory: " + rel);
            }
            ++result.entriesWritten;
            // Apply directory mtime if present.
            if (archive_entry_mtime_is_set(entry)) {
                HANDLE h = CreateFileW(
                    fullExt.c_str(), FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    FILETIME ft = UnixToFileTime(archive_entry_mtime(entry));
                    SetFileTime(h, nullptr, nullptr, &ft);
                    CloseHandle(h);
                }
            }
            continue;
        }

        // Regular file: ensure parent dirs exist.
        const std::size_t sep = fullPath.find_last_of(L'\\');
        if (sep != std::wstring::npos) {
            const std::wstring parent = fullPath.substr(0, sep);
            if (parent.size() > stagingRoot.size() &&
                !CreateDirsRecursive(parent)) {
                return Fail(ExtractStatus::WriteFailed,
                            L"cannot create parent of: " + rel);
            }
        }

        HANDLE h = CreateFileW(fullExt.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            return Fail(e == ERROR_DISK_FULL ? ExtractStatus::WriteFailed
                                             : ExtractStatus::WriteFailed,
                        L"cannot create file: " + rel);
        }

        // --- Stream the data in chunks, checking cancellation --------------
        bool writeOk = true;
        for (;;) {
            const void* buf = nullptr;
            std::size_t len = 0;
            la_int64_t offset = 0;
            const int rd = archive_read_data_block(a, &buf, &len, &offset);
            if (rd == ARCHIVE_EOF) break;
            if (rd < ARCHIVE_WARN) {
                CloseHandle(h);
                DeleteFileW(fullExt.c_str());
                return Fail(ExtractStatus::CorruptInput,
                            L"read error in entry " + rel + L": " +
                                Widen(ErrText(a)));
            }
            if (callbacks.cancel && callbacks.cancel->IsCancelled()) {
                CloseHandle(h);
                DeleteFileW(fullExt.c_str());
                return Fail(ExtractStatus::Cancelled, L"cancelled");
            }
            const char* p = static_cast<const char*>(buf);
            std::size_t remaining = len;
            while (remaining > 0) {
                DWORD wrote = 0;
                const DWORD chunk = static_cast<DWORD>(
                    std::min<std::size_t>(remaining, 1u << 20));
                if (!WriteFile(h, p, chunk, &wrote, nullptr) || wrote == 0) {
                    writeOk = false;
                    break;
                }
                p += wrote;
                remaining -= wrote;
                bytesWritten += wrote;
            }
            if (!writeOk) break;

            // Progress: single-stream by compressed bytes read; containers by
            // uncompressed bytes written.
            if (singleStream) {
                const std::uint64_t consumed =
                    static_cast<std::uint64_t>(archive_filter_bytes(a, -1));
                progress.Emit(consumed, sourceSize, entryName);
            } else {
                progress.Emit(bytesWritten, 0, entryName);
            }
        }

        if (!writeOk) {
            const DWORD e = GetLastError();
            CloseHandle(h);
            DeleteFileW(fullExt.c_str());
            return Fail(ExtractStatus::WriteFailed,
                        e == ERROR_DISK_FULL
                            ? (L"disk full writing: " + rel)
                            : (L"write failed: " + rel));
        }

        // Apply modified time (clamped) before closing.
        if (archive_entry_mtime_is_set(entry)) {
            FILETIME ft = UnixToFileTime(archive_entry_mtime(entry));
            SetFileTime(h, nullptr, nullptr, &ft);
        }
        CloseHandle(h);
        ++result.entriesWritten;
        progress.Emit(bytesWritten,
                      singleStream ? sourceSize : std::uint64_t{0}, entryName);
    }

    // Final progress beat.
    progress.Emit(singleStream ? sourceSize : bytesWritten,
                  singleStream ? sourceSize : bytesWritten, L"", /*force=*/true);

    result.status = ExtractStatus::Ok;
    return result;
}

// ---------------------------------------------------------------------------
// Temp-staging utilities
// ---------------------------------------------------------------------------

std::wstring CreateStagingDir(std::wstring_view workDir) {
    // Derive a deterministic-source id from a GUID (CoCreateGuid). This is a
    // process/GUID source, not Math.random-style nondeterminism.
    GUID guid;
    if (FAILED(CoCreateGuid(&guid))) {
        // Fallback: PID + tick — still a process source, never plain rand().
        guid = GUID{};
        guid.Data1 = GetCurrentProcessId();
        guid.Data2 = static_cast<unsigned short>(GetTickCount64() & 0xFFFF);
    }
    wchar_t hex[33];
    swprintf(hex, 33, L"%08lx%04hx%04hx%016llx", guid.Data1, guid.Data2,
             guid.Data3,
             *reinterpret_cast<const unsigned long long*>(guid.Data4));

    const std::wstring base = NormalizeSeparators(workDir);
    const std::wstring name = std::wstring(L".archive-extractor-tmp-") + hex;
    const std::wstring full = JoinPath(base, name);
    const std::wstring ext = ToExtendedPath(full);

    if (!CreateDirectoryW(ext.c_str(), nullptr)) {
        return {};
    }
    // Hide it.
    DWORD attr = GetFileAttributesW(ext.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        SetFileAttributesW(ext.c_str(), attr | FILE_ATTRIBUTE_HIDDEN);
    }
    return full;
}

bool RemoveDirTree(std::wstring_view dir) {
    const std::wstring root = NormalizeSeparators(dir);
    if (root.empty()) return true;

    const std::wstring rootExt = ToExtendedPath(root);
    const DWORD attr = GetFileAttributesW(rootExt.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return true;  // already gone
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        // Clear read-only just in case, then delete as a file.
        SetFileAttributesW(rootExt.c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(rootExt.c_str()) != 0;
    }

    // Enumerate and recurse.
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = JoinPath(root, L"*");
    HANDLE find = FindFirstFileW(ToExtendedPath(pattern).c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring nm = fd.cFileName;
            if (nm == L"." || nm == L"..") continue;
            const std::wstring child = JoinPath(root, nm);
            const std::wstring childExt = ToExtendedPath(child);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirTree(child);
            } else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
                    SetFileAttributesW(childExt.c_str(), FILE_ATTRIBUTE_NORMAL);
                }
                DeleteFileW(childExt.c_str());
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    // Clear hidden/read-only on the dir itself so RemoveDirectory succeeds.
    SetFileAttributesW(rootExt.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!RemoveDirectoryW(rootExt.c_str())) {
        return GetFileAttributesW(rootExt.c_str()) == INVALID_FILE_ATTRIBUTES;
    }
    return true;
}

bool SameVolume(std::wstring_view a, std::wstring_view b) {
    const std::wstring ra = VolumeRoot(a);
    const std::wstring rb = VolumeRoot(b);
    if (ra.empty() || rb.empty()) return false;
    // Compare via GetVolumePathName for robustness across case / mount points.
    std::array<wchar_t, MAX_PATH> va{}, vb{};
    const bool oka =
        GetVolumePathNameW(ToExtendedPath(a).c_str(), va.data(),
                           static_cast<DWORD>(va.size())) != 0;
    const bool okb =
        GetVolumePathNameW(ToExtendedPath(b).c_str(), vb.data(),
                           static_cast<DWORD>(vb.size())) != 0;
    if (oka && okb) {
        return _wcsicmp(va.data(), vb.data()) == 0;
    }
    return _wcsicmp(ra.c_str(), rb.c_str()) == 0;
}

// ---------------------------------------------------------------------------
// Worker-thread driver
// ---------------------------------------------------------------------------

std::thread RunExtractionOnWorker(IExtractor& extractor,
                                  const PipelinePlan& plan,
                                  std::wstring sourcePath,
                                  std::wstring stagingDir,
                                  ExtractCallbacks callbacks,
                                  std::function<void(ExtractResult)> onDone) {
    return std::thread([&extractor, plan, sourcePath = std::move(sourcePath),
                        stagingDir = std::move(stagingDir),
                        callbacks = std::move(callbacks),
                        onDone = std::move(onDone)]() mutable {
        ExtractResult res =
            extractor.extractToStaging(plan, sourcePath, stagingDir, callbacks);
        if (onDone) onDone(std::move(res));
    });
}

}  // namespace ae
