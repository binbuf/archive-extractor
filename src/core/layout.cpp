#include "archive_core/layout.h"

#include <algorithm>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "archive_core/extract.h"  // ToExtendedPath, RemoveDirTree, SameVolume
#include "archive_core/logging.h"
#include "archive_core/retry.h"

namespace ae {

namespace {

bool IsSeparator(wchar_t c) { return c == L'\\' || c == L'/'; }

std::wstring NormalizeSeparators(std::wstring_view s) {
    std::wstring out(s);
    std::replace(out.begin(), out.end(), L'/', L'\\');
    return out;
}

// Drop a single trailing separator (but keep a bare "C:\" root intact).
std::wstring StripTrailingSep(std::wstring_view s) {
    std::wstring out = NormalizeSeparators(s);
    while (out.size() > 1 && IsSeparator(out.back()) &&
           !(out.size() == 3 && out[1] == L':')) {
        out.pop_back();
    }
    return out;
}

std::wstring JoinPath(std::wstring_view base, std::wstring_view child) {
    std::wstring out(base);
    if (!out.empty() && !IsSeparator(out.back())) out.push_back(L'\\');
    out.append(child);
    return out;
}

bool PathExists(std::wstring_view normalPath) {
    return GetFileAttributesW(ToExtendedPath(normalPath).c_str()) !=
           INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(std::wstring_view normalPath) {
    const DWORD attr = GetFileAttributesW(ToExtendedPath(normalPath).c_str());
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Split a leaf file name into (stem, extension) at the LAST dot, matching how
// Windows Explorer inserts a "(n)" counter. A leading dot (dotfile like
// ".gitignore") is treated as having no extension. The returned `ext` includes
// the leading dot, or is empty.
void SplitFileExt(std::wstring_view name, std::wstring& stem,
                  std::wstring& ext) {
    const std::size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) {
        stem.assign(name);
        ext.clear();
    } else {
        stem.assign(name.substr(0, dot));
        ext.assign(name.substr(dot));
    }
}

// Enumerate the top-level (root) entries of a directory. Returns the leaf names
// only. The order is whatever the filesystem yields; the caller only needs the
// count and, for the 1-entry case, the single name.
std::vector<std::wstring> RootEntries(std::wstring_view dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = JoinPath(StripTrailingSep(dir), L"*");
    HANDLE find = FindFirstFileW(ToExtendedPath(pattern).c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring nm = fd.cFileName;
        if (nm == L"." || nm == L"..") continue;
        out.push_back(nm);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return out;
}

}  // namespace

std::wstring MakeNonCollidingName(std::wstring_view dir, std::wstring_view name,
                                  bool isFile) {
    const std::wstring base = StripTrailingSep(dir);

    // First try the unmodified name.
    if (!PathExists(JoinPath(base, name))) {
        return std::wstring(name);
    }

    std::wstring stem, ext;
    if (isFile) {
        SplitFileExt(name, stem, ext);
    } else {
        stem.assign(name);  // folders: counter appended to the whole name
    }

    for (unsigned n = 1; n != 0; ++n) {
        std::wstring candidate =
            stem + L" (" + std::to_wstring(n) + L")" + ext;
        if (!PathExists(JoinPath(base, candidate))) {
            return candidate;
        }
    }
    // Astronomically unreachable; return the last probe to avoid overwrite.
    return std::wstring(name);
}

namespace {

PlaceResult Fail(PlaceStatus st, std::wstring msg, std::size_t count = 0,
                 unsigned long lastError = 0) {
    PlaceResult r;
    r.status = st;
    r.message = std::move(msg);
    r.rootEntryCount = count;
    r.lastError = lastError;
    return r;
}

// Move `from` (a normal path) to `to` (a normal path) via a same-volume rename.
// Both are extended-path-prefixed for the call. No overwrite flag is set — the
// caller guarantees `to` is free (collision probing done up front).
//
// A just-created destination can be transiently locked by AV / the indexer
// (design 06 "File in use / locked on move"; 07 §14), so the rename is run
// through RetryWithBackoff: it retries only on transient lock errors, then
// reports the final Win32 error via `outErr` so the caller can classify it.
bool RenameMove(std::wstring_view from, std::wstring_view to,
                unsigned long& outErr) {
    const std::wstring f = ToExtendedPath(from);
    const std::wstring t = ToExtendedPath(to);
    // No MOVEFILE_REPLACE_EXISTING: never overwrite. MOVEFILE_COPY_ALLOWED is
    // deliberately OMITTED so this stays an O(1) rename and fails loudly if the
    // dirs are unexpectedly cross-volume (which would otherwise silently copy).
    unsigned long lastErr = 0;
    const bool ok = RetryWithBackoff([&](unsigned long& err) {
        if (MoveFileExW(f.c_str(), t.c_str(), 0)) return true;
        err = GetLastError();
        lastErr = err;  // remember the final failure for the caller
        return false;
    });
    outErr = ok ? 0 : lastErr;
    return ok;
}

}  // namespace

PlaceResult LayoutPlanner::place(std::wstring_view stagingDir,
                                 std::wstring_view workDir,
                                 std::wstring_view stem, Kind kind) const {
    const std::wstring staging = StripTrailingSep(stagingDir);
    const std::wstring work = StripTrailingSep(workDir);

    if (staging.empty() || work.empty()) {
        return Fail(PlaceStatus::BadInput, L"empty staging/work dir");
    }
    if (!IsDirectory(staging)) {
        return Fail(PlaceStatus::BadInput, L"staging dir is missing");
    }

    // Inspect the staged root (top level only).
    std::vector<std::wstring> entries = RootEntries(staging);
    const std::size_t count = entries.size();

    // --- Empty archive: no output, NO wrapper folder, clean up --------------
    // Silent completion (implementer's choice, design 06): there is nothing to
    // place, so we simply remove the temp dir and report NoOutput.
    if (count == 0) {
        RemoveDirTree(staging);
        PlaceResult r;
        r.status = PlaceStatus::NoOutput;
        r.rootEntryCount = 0;
        return r;
    }

    // Decide the target leaf name and whether it is a file.
    // 1 entry (or any single-stream output) -> the entry keeps its OWN name and
    //   is placed directly (no <stem> wrapper).
    // 2+ entries -> wrap in a folder named `stem`.
    const bool singleEntry = (count == 1) || (kind == Kind::SingleStream);

    std::wstring desiredName;
    bool targetIsFile = false;
    std::wstring sourceItem;  // the staging-side path that gets moved

    if (singleEntry) {
        const std::wstring& leaf = entries.front();
        desiredName = leaf;
        sourceItem = JoinPath(staging, leaf);
        targetIsFile = !IsDirectory(sourceItem);
    } else {
        // Wrap: the staging dir itself becomes the <stem> folder. Rename the
        // whole temp dir to <work>\<stem> — an O(1) move that carries every
        // root entry inside with it.
        desiredName = stem.empty() ? std::wstring(L"archive") : std::wstring(stem);
        sourceItem = staging;
        targetIsFile = false;
    }

    // --- Collision probing BEFORE any move (transactional) ------------------
    const std::wstring finalName =
        MakeNonCollidingName(work, desiredName, targetIsFile);
    const bool renamed = (finalName != desiredName);
    const std::wstring target = JoinPath(work, finalName);

    // --- The atomic move ----------------------------------------------------
    unsigned long moveErr = 0;
    if (!RenameMove(sourceItem, target, moveErr)) {
        // Leave the working dir untouched: remove the temp tree on any failure.
        RemoveDirTree(staging);
        return Fail(PlaceStatus::PlaceFailed,
                    L"placement move failed (error " +
                        std::to_wstring(moveErr) + L")",
                    count, moveErr);
    }

    // --- Cleanup ------------------------------------------------------------
    // 1-entry path: the moved item left the temp dir behind (now empty) -> drop
    //   it. 2+ path: the temp dir itself BECAME the placed folder, so there is
    //   nothing left to remove.
    if (singleEntry) {
        RemoveDirTree(staging);
    } else {
        // The 2+-entry path renames the HIDDEN staging dir into place, so the
        // placed wrapper folder would otherwise inherit the hidden/system bits.
        // Clear them so the result is a normal, visible folder.
        const std::wstring t = ToExtendedPath(target);
        const DWORD attr = GetFileAttributesW(t.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES &&
            (attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) {
            SetFileAttributesW(
                t.c_str(),
                attr & ~(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
        }
    }

    if (renamed) {
        Log(L"auto-renamed placed item to: " + finalName);
    }

    PlaceResult r;
    r.status = PlaceStatus::Ok;
    r.placedPath = target;
    r.renamed = renamed;
    r.rootEntryCount = count;
    return r;
}

}  // namespace ae
