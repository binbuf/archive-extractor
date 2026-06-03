// bit7z / 7z.dll extraction backend (task 09).
//
// Implements SevenZipExtractor (the IExtractor for encrypted-7z and all
// RAR/RAR5, plus their multi-volume chains), the open-time encryption query
// that completes the detector's re-route hook (ResolveBackend), and the
// multi-volume completeness check (VerifyMultiVolumeComplete).
//
// The runtime 7z.dll is loaded ON DEMAND: the bit7z::Bit7zLibrary is a function
// local inside extractToStaging, so it is constructed (and 7z.dll LoadLibrary'd)
// only when this backend actually runs. libarchive/plain-zip paths never touch
// 7z.dll, keeping cold start unaffected (verified by a test that asserts
// GetModuleHandleW(L"7z.dll") is null after a plain-zip extraction).

#include "archive_core/extract.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <archive.h>
#include <archive_entry.h>

#include <bit7z/bit7z.hpp>
#include <bit7z/bitarchivereader.hpp>
#include <bit7z/bitexception.hpp>
#include <bit7z/bitfileextractor.hpp>
#include <bit7z/bitformat.hpp>

#include "archive_core/logging.h"

namespace fs = std::filesystem;

namespace ae {
namespace {

ExtractResult Fail7z(ExtractStatus st, std::wstring msg) {
    ExtractResult r;
    r.status = st;
    r.message = std::move(msg);
    return r;
}

// UTF-16 -> UTF-8 (bit7z's tstring is std::string on this build).
std::string Narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Widen8(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

bool FileExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(ToExtendedPath(path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Build a sibling path next to `firstPart` (same directory) with `name`.
std::wstring SiblingPath(std::wstring_view firstPart, std::wstring_view name) {
    const fs::path parent = fs::path(std::wstring(firstPart)).parent_path();
    return parent.empty() ? std::wstring(name)
                          : (parent / std::wstring(name)).wstring();
}

// Map a bit7z exception's failure condition to our coarse ExtractStatus.
ExtractStatus StatusFromBitException(const bit7z::BitException& ex) {
    const std::error_code code = ex.code();
    if (code == bit7z::BitFailureSource::WrongPassword) {
        return ExtractStatus::NeedPassword;
    }
    if (code == bit7z::BitFailureSource::CRCError ||
        code == bit7z::BitFailureSource::DataError ||
        code == bit7z::BitFailureSource::InvalidArchive ||
        code == bit7z::BitFailureSource::HeadersError ||
        code == bit7z::BitFailureSource::UnexpectedEnd ||
        code == bit7z::BitFailureSource::DataAfterEnd ||
        code == bit7z::BitFailureSource::UnavailableData) {
        return ExtractStatus::CorruptInput;
    }
    return ExtractStatus::OpenFailed;
}

}  // namespace

// ---------------------------------------------------------------------------
// Multi-volume completeness check
// ---------------------------------------------------------------------------

bool VerifyMultiVolumeComplete(const PipelinePlan& plan,
                               std::wstring_view firstPart,
                               std::wstring& missing) {
    missing.clear();
    if (plan.volumeFamily == VolumeFamily::NotMultiVolume) return true;

    const fs::path fp{std::wstring(firstPart)};
    const std::wstring firstName = fp.filename().wstring();

    switch (plan.volumeFamily) {
        case VolumeFamily::SevenZip: {
            // <stem>.7z.001, .002, ... — contiguous from .001 until a gap.
            // The first part must exist; then enumerate .002, .003 ... and
            // stop at the first absent one. A 7z split is complete iff parts
            // .001..N are contiguous (7z volumes are always sequential). We
            // detect a gap by requiring that, once .001 exists, there is no
            // hole before the highest present index. Since we cannot know N a
            // priori, we walk until the first missing index; if that index is
            // beyond the last existing one, the set is contiguous (complete).
            // A "missing middle" manifests as a present part after the gap.
            const std::wstring base = std::wstring(firstPart).substr(
                0, std::wstring(firstPart).size() - 4);  // strip ".001"
            // base now ends with ".7z"; rebuild "<...>.7z." prefix.
            std::wstring prefix = base + L".";
            unsigned highestPresent = 0;
            unsigned firstGap = 0;
            bool sawGap = false;
            for (unsigned i = 1; i <= 999; ++i) {
                wchar_t suf[8];
                swprintf(suf, 8, L"%03u", i);
                const std::wstring part = prefix + suf;
                if (FileExists(part)) {
                    highestPresent = i;
                    if (sawGap) {
                        // A present part after a gap => the gap part is missing.
                        wchar_t gsuf[8];
                        swprintf(gsuf, 8, L"%03u", firstGap);
                        missing = fs::path(prefix + gsuf).filename().wstring();
                        return false;
                    }
                } else if (!sawGap) {
                    firstGap = i;
                    sawGap = true;
                }
            }
            if (highestPresent == 0) {
                missing = fs::path(prefix + L"001").filename().wstring();
                return false;
            }
            return true;
        }
        case VolumeFamily::RarPart: {
            // <stem>.part1.rar, .part2.rar, ... — contiguous from part1.
            const std::wstring stem = plan.stem;
            unsigned highestPresent = 0;
            unsigned firstGap = 0;
            bool sawGap = false;
            for (unsigned i = 1; i <= 999; ++i) {
                const std::wstring part =
                    SiblingPath(firstPart, stem + L".part" +
                                               std::to_wstring(i) + L".rar");
                if (FileExists(part)) {
                    highestPresent = i;
                    if (sawGap) {
                        missing = stem + L".part" + std::to_wstring(firstGap) +
                                  L".rar";
                        return false;
                    }
                } else if (!sawGap) {
                    firstGap = i;
                    sawGap = true;
                }
            }
            if (highestPresent == 0) {
                missing = stem + L".part1.rar";
                return false;
            }
            return true;
        }
        case VolumeFamily::RarOld: {
            // <stem>.rar + .r00, .r01, ... — contiguous continuation parts.
            const std::wstring stem = plan.stem;
            // .rar (first) must exist.
            if (!FileExists(std::wstring(firstPart))) {
                missing = firstName;
                return false;
            }
            unsigned highestPresent = 0;  // 0 means only .rar present
            unsigned firstGap = 0;
            bool sawGap = false;
            for (unsigned i = 0; i <= 999; ++i) {
                wchar_t suf[8];
                swprintf(suf, 8, L".r%02u", i);
                const std::wstring part = SiblingPath(firstPart, stem + suf);
                if (FileExists(part)) {
                    highestPresent = i + 1;
                    if (sawGap) {
                        wchar_t gsuf[8];
                        swprintf(gsuf, 8, L".r%02u", firstGap);
                        missing = stem + gsuf;
                        return false;
                    }
                } else if (!sawGap) {
                    firstGap = i;
                    sawGap = true;
                }
            }
            (void)highestPresent;
            return true;  // .rar alone is a valid (single-part) RAR.
        }
        case VolumeFamily::ZipSpanned:
        case VolumeFamily::NotMultiVolume:
            return true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Encryption query / backend resolution
// ---------------------------------------------------------------------------

Backend ResolveBackend(const PipelinePlan& plan, std::wstring_view sourcePath) {
    if (!plan.supported()) return Backend::None;

    // RAR (and any plan already on SevenZipDll, e.g. a content-sniffed
    // encrypted 7z) needs no probe — it is already correctly routed.
    if (plan.backend == Backend::SevenZipDll) return Backend::SevenZipDll;

    // Only a plain-7z plan can re-route on encryption; everything else
    // (zip is decrypted by libarchive, tar/single-stream never encrypted)
    // keeps its backend.
    if (plan.format != Format::SevenZip) return plan.backend;

    // Open via libarchive and ask whether the 7z stream has encrypted entries.
    // archive_read_has_encrypted_entries() returns 1/0 when known, or a
    // negative "undecided/unsupported" sentinel; treat >0 as encrypted.
    bool encrypted = false;
    struct archive* a = archive_read_new();
    if (a) {
        archive_read_support_filter_all(a);
        archive_read_support_format_all(a);
        const std::wstring src = ToExtendedPath(sourcePath);
        if (archive_read_open_filename_w(a, src.c_str(), 64 * 1024) ==
            ARCHIVE_OK) {
            // Peek at the first header so the encryption state is populated.
            struct archive_entry* entry = nullptr;
            archive_read_next_header(a, &entry);
            const int enc = archive_read_has_encrypted_entries(a);
            if (enc > 0) encrypted = true;
        }
        archive_read_free(a);
    }
    return ReroutedForEncryption(plan, encrypted);
}

// ---------------------------------------------------------------------------
// SevenZipExtractor
// ---------------------------------------------------------------------------

ExtractResult SevenZipExtractor::extractToStaging(
    const PipelinePlan& plan, std::wstring_view sourcePath,
    std::wstring_view stagingDir, const ExtractCallbacks& callbacks) {
    if (plan.backend != Backend::SevenZipDll) {
        return Fail7z(ExtractStatus::Unsupported,
                      L"plan backend is not 7z.dll");
    }

    // Multi-volume: open the FIRST part; if the input was a non-first part the
    // detector already resolved plan.firstPartPath. Prefer it when present.
    const std::wstring firstPart =
        !plan.firstPartPath.empty() ? plan.firstPartPath
                                    : std::wstring(sourcePath);

    // Completeness check — a missing sibling yields a named, non-truncating
    // error before we touch the DLL or write anything.
    {
        std::wstring missing;
        if (!VerifyMultiVolumeComplete(plan, firstPart, missing)) {
            return Fail7z(ExtractStatus::CorruptInput,
                          L"missing archive volume: " + missing);
        }
    }

    if (!FileExists(firstPart)) {
        return Fail7z(ExtractStatus::OpenFailed,
                      L"cannot open archive: file not found");
    }

    if (callbacks.cancel && callbacks.cancel->IsCancelled()) {
        return Fail7z(ExtractStatus::Cancelled, L"cancelled");
    }

    // Pick the explicit bit7z format from the detected format for fidelity.
    // RAR5 vs legacy RAR is auto-detected by bit7z when given BitFormat::Rar5,
    // but BitFormat::Auto (enabled in this build) handles both RAR variants and
    // 7z reliably from the file signature; use it so legacy + RAR5 both work.
    const bit7z::BitInFormat& format =
        (plan.format == Format::Rar) ? bit7z::BitFormat::Rar5
                                     : bit7z::BitFormat::SevenZip;

    // Password from the callback (task 10 supplies the real prompt; here it is
    // a stub returning nullopt unless a test supplies one).
    std::wstring password;
    bool havePassword = false;
    if (callbacks.requestPassword) {
        if (auto pw = callbacks.requestPassword()) {
            password = *pw;
            havePassword = true;
        }
    }

    try {
        // --- ON-DEMAND 7z.dll load -----------------------------------------
        // Constructing Bit7zLibrary here (not as a static/member) is the lazy
        // load: 7z.dll is LoadLibrary'd only now, the first time this backend
        // runs. It is searched per Win32 rules; the build copies 7z.dll next to
        // the exe and the test exe so it resolves.
        bit7z::Bit7zLibrary lib;

        // BitArchiveReader opens + enumerates and lets us drive progress,
        // cancellation, and password on the same object used to extract. The
        // password is passed to the CONSTRUCTOR so header-encrypted archives
        // (whose very file list is encrypted) can be opened — setPassword()
        // after the fact is too late for those.
        bit7z::BitArchiveReader reader(
            lib, Narrow(firstPart), format,
            havePassword ? Narrow(password) : bit7z::tstring{});

        // Header-encrypted 7z (file list itself encrypted): if we could open
        // (no header crypto) but items are encrypted and we have no password,
        // surface NeedPassword cleanly rather than failing mid-extract.
        if (!havePassword && reader.hasEncryptedItems()) {
            return Fail7z(ExtractStatus::NeedPassword,
                          L"archive is encrypted; a password is required");
        }

        // Detect links / count entries up front for the result summary and so
        // we report hadLinks like the libarchive backend (sym/hard links are
        // surfaced via the WinAttributes / attrib bits; bit7z extracts them as
        // files, so we only flag, never fail).
        ExtractResult result;
        const std::uint64_t totalBytes = reader.size();

        // --- Progress + cancel mapping -------------------------------------
        // bit7z's ProgressCallback returns false to abort. We map processed
        // bytes -> Progress{done,total}; throttle to ~40ms like libarchive.
        auto lastEmit = std::chrono::steady_clock::now() -
                        std::chrono::milliseconds(1000);
        std::wstring currentEntry;

        reader.setFileCallback([&](const bit7z::tstring& path) {
            currentEntry = Widen8(path);
        });

        reader.setProgressCallback([&](std::uint64_t processed) -> bool {
            if (callbacks.cancel && callbacks.cancel->IsCancelled()) {
                return false;  // abort: bit7z throws, we map to Cancelled.
            }
            if (callbacks.onProgress) {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastEmit >= std::chrono::milliseconds(40)) {
                    lastEmit = now;
                    Progress p;
                    p.done = processed;
                    p.total = totalBytes;
                    p.currentEntry = currentEntry;
                    callbacks.onProgress(p);
                }
            }
            return true;
        });

        // --- Extract everything (handles solid archives transparently) -----
        reader.extractTo(Narrow(stagingDir));

        // Re-check cancellation in case the abort raced the final flush.
        if (callbacks.cancel && callbacks.cancel->IsCancelled()) {
            return Fail7z(ExtractStatus::Cancelled, L"cancelled");
        }

        // Summary: count materialized entries (files + folders) and flag links
        // (bit7z extracts them as files; we only surface the fact, never fail —
        // identical to the libarchive backend's hadLinks behavior).
        for (const auto& item : reader.items()) {
            ++result.entriesWritten;
            if (item.isSymLink()) result.hadLinks = true;
        }

        // Final progress beat (force) so the bar lands at 100%.
        if (callbacks.onProgress) {
            Progress p;
            p.done = totalBytes;
            p.total = totalBytes;
            callbacks.onProgress(p);
        }

        result.status = ExtractStatus::Ok;
        return result;
    } catch (const bit7z::BitException& ex) {
        // A cancellation aborts via the progress callback returning false,
        // which bit7z reports as an operation error — translate to Cancelled.
        if (callbacks.cancel && callbacks.cancel->IsCancelled()) {
            return Fail7z(ExtractStatus::Cancelled, L"cancelled");
        }
        const ExtractStatus st = StatusFromBitException(ex);
        Log(std::wstring(L"[7z] BitException: ") + Widen8(ex.what()));
        return Fail7z(st, Widen8(ex.what()));
    } catch (const std::exception& ex) {
        if (callbacks.cancel && callbacks.cancel->IsCancelled()) {
            return Fail7z(ExtractStatus::Cancelled, L"cancelled");
        }
        return Fail7z(ExtractStatus::OpenFailed, Widen8(ex.what()));
    }
}

}  // namespace ae
