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
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

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

// Detect whether a zip uses WinZip-AES encryption (compression method 99) on
// any entry. The bundled libarchive build lacks the AES cipher (no working
// PBKDF2/AES), so AES-encrypted zips must be routed to the 7z.dll backend, which
// decrypts them natively; traditional ZipCrypto stays on libarchive. We walk the
// central directory (fixed-size records, no interleaved data) looking for an
// encrypted entry (general-purpose bit 0) with method 99. Best-effort: any parse
// failure (or ZIP64-only layout) returns false and leaves routing unchanged.
bool ZipUsesAesEncryption(std::wstring_view path) {
    HANDLE h = CreateFileW(ToExtendedPath(std::wstring(path)).c_str(),
                           GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) {
        CloseHandle(h);
        return false;
    }
    const long long fileSize = sz.QuadPart;

    auto rd16 = [](const unsigned char* p) -> std::uint16_t {
        return static_cast<std::uint16_t>(p[0]) |
               static_cast<std::uint16_t>(p[1] << 8);
    };
    auto rd32 = [](const unsigned char* p) -> std::uint32_t {
        return static_cast<std::uint32_t>(p[0]) |
               (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) |
               (static_cast<std::uint32_t>(p[3]) << 24);
    };

    // Read the tail (End Of Central Directory lives within the last 64 KiB+22).
    const long long kMaxTail = 65557;
    const DWORD tailLen =
        static_cast<DWORD>(fileSize < kMaxTail ? fileSize : kMaxTail);
    std::vector<unsigned char> tail(tailLen);
    LARGE_INTEGER off;
    off.QuadPart = fileSize - tailLen;
    SetFilePointerEx(h, off, nullptr, FILE_BEGIN);
    DWORD got = 0;
    if (!ReadFile(h, tail.data(), tailLen, &got, nullptr) || got < 22) {
        CloseHandle(h);
        return false;
    }

    long long eocd = -1;
    for (long long i = static_cast<long long>(got) - 22; i >= 0; --i) {
        if (rd32(&tail[static_cast<std::size_t>(i)]) == 0x06054b50UL) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        CloseHandle(h);
        return false;
    }
    const std::uint32_t cdSize = rd32(&tail[static_cast<std::size_t>(eocd) + 12]);
    const std::uint32_t cdOffset =
        rd32(&tail[static_cast<std::size_t>(eocd) + 16]);
    if (cdSize == 0 || cdSize == 0xFFFFFFFFUL || cdOffset == 0xFFFFFFFFUL) {
        CloseHandle(h);  // empty or ZIP64 — leave routing unchanged
        return false;
    }

    std::vector<unsigned char> cd(cdSize);
    LARGE_INTEGER cdo;
    cdo.QuadPart = cdOffset;
    SetFilePointerEx(h, cdo, nullptr, FILE_BEGIN);
    DWORD cdGot = 0;
    const bool ok = ReadFile(h, cd.data(), cdSize, &cdGot, nullptr) != 0;
    CloseHandle(h);
    if (!ok) return false;
    cd.resize(cdGot);

    std::size_t i = 0;
    while (i + 46 <= cd.size()) {
        if (rd32(&cd[i]) != 0x02014b50UL) break;  // central-dir file header
        const std::uint16_t flag = rd16(&cd[i + 8]);
        const std::uint16_t method = rd16(&cd[i + 10]);
        const std::uint16_t fnLen = rd16(&cd[i + 28]);
        const std::uint16_t extraLen = rd16(&cd[i + 30]);
        const std::uint16_t commentLen = rd16(&cd[i + 32]);
        if ((flag & 0x0001) && method == 99) return true;  // WinZip AES
        i += 46u + fnLen + extraLen + commentLen;
    }
    return false;
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

    // Encrypted WinZip-AES zip: the bundled libarchive lacks the AES cipher, so
    // route AES-encrypted zips to the 7z.dll backend (it decrypts both ZipCrypto
    // and AES). Traditional ZipCrypto stays on libarchive's passphrase path.
    if (plan.format == Format::Zip) {
        if (ZipUsesAesEncryption(sourcePath)) return Backend::SevenZipDll;
        return plan.backend;
    }

    // Only a plain-7z plan can re-route on encryption; everything else
    // (tar/single-stream is never encrypted) keeps its backend.
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
        (plan.format == Format::Rar)   ? bit7z::BitFormat::Rar5
        : (plan.format == Format::Zip) ? bit7z::BitFormat::Zip
                                       : bit7z::BitFormat::SevenZip;

    // Password state. The accepted passphrase is cached here for the lifetime of
    // this one extraction (reused for every entry, never re-prompted per file,
    // never reused across processes) and zeroed on every exit path.
    std::wstring password;
    bool havePassword = false;
    unsigned attempt = 0;
    const std::wstring archiveName =
        fs::path(std::wstring(firstPart)).filename().wstring();

    // Zero the password buffer on any return/throw out of this function.
    struct PwZeroizer {
        std::wstring* p;
        ~PwZeroizer() {
            if (!p->empty()) SecureZeroMemory(p->data(), p->size() * sizeof(wchar_t));
        }
    } pwZero{&password};

    try {
        // --- ON-DEMAND 7z.dll load -----------------------------------------
        // Constructing Bit7zLibrary here (not as a static/member) is the lazy
        // load: 7z.dll is LoadLibrary'd only now, the first time this backend
        // runs. It is searched per Win32 rules; the build copies 7z.dll next to
        // the exe and the test exe so it resolves.
        bit7z::Bit7zLibrary lib;

        // Drive an opened reader's extraction into staging with progress +
        // cancellation. Returns Ok on success; on a WRONG password thrown during
        // extraction (clear-header archives only fail at decrypt time) it
        // returns the NeedPassword sentinel so the retry loop can re-prompt;
        // cancellation maps to Cancelled. Any other BitException propagates to
        // the outer handler as a genuine error.
        auto doExtract =
            [&](bit7z::BitArchiveReader& reader) -> ExtractResult {
            ExtractResult result;
            const std::uint64_t totalBytes = reader.size();

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

            try {
                // Extract everything (handles solid archives transparently).
                reader.extractTo(Narrow(stagingDir));
            } catch (const bit7z::BitException& ex) {
                if (callbacks.cancel && callbacks.cancel->IsCancelled()) {
                    return Fail7z(ExtractStatus::Cancelled, L"cancelled");
                }
                if (ex.code() == bit7z::BitFailureSource::WrongPassword) {
                    return Fail7z(ExtractStatus::NeedPassword,
                                  L"incorrect password");
                }
                throw;  // genuine error: outer handler maps + logs it.
            }

            // Re-check cancellation in case the abort raced the final flush.
            if (callbacks.cancel && callbacks.cancel->IsCancelled()) {
                return Fail7z(ExtractStatus::Cancelled, L"cancelled");
            }

            // Summary: count materialized entries (files + folders) and flag
            // links (bit7z extracts them as files; we only surface the fact,
            // never fail — identical to the libarchive backend's hadLinks).
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
        };

        // Remove any partial output a wrong-password attempt left in staging,
        // then recreate the (empty) staging dir for the next attempt.
        auto clearStaging = [&]() {
            RemoveDirTree(stagingDir);
            CreateDirectoryW(ToExtendedPath(stagingDir).c_str(), nullptr);
        };

        // --- Retry / cap / cache loop --------------------------------------
        for (;;) {
            if (callbacks.cancel && callbacks.cancel->IsCancelled()) {
                return Fail7z(ExtractStatus::Cancelled, L"cancelled");
            }

            // Open (and, for header-encrypted archives, decrypt the file list).
            // The password is passed to the CONSTRUCTOR so header-encrypted
            // archives — whose very listing is encrypted — can be opened at all;
            // setPassword() after the fact is too late for those.
            bool openWrongPassword = false;
            std::unique_ptr<bit7z::BitArchiveReader> reader;
            try {
                reader = std::make_unique<bit7z::BitArchiveReader>(
                    lib, Narrow(firstPart), format,
                    havePassword ? Narrow(password) : bit7z::tstring{});
            } catch (const bit7z::BitException& ex) {
                if (callbacks.cancel && callbacks.cancel->IsCancelled()) {
                    return Fail7z(ExtractStatus::Cancelled, L"cancelled");
                }
                // A header-encrypted archive (file list itself encrypted) throws
                // right here when opened without a password — or with the wrong
                // one. Either way the remedy is to (re-)prompt; the prompt
                // section below surfaces NeedPassword when there is no callback
                // or the retry cap is hit. Any other open failure is terminal.
                if (ex.code() == bit7z::BitFailureSource::WrongPassword) {
                    openWrongPassword = true;
                } else {
                    Log(std::wstring(L"[7z] BitException: ") + Widen8(ex.what()));
                    return Fail7z(StatusFromBitException(ex), Widen8(ex.what()));
                }
            }

            if (!openWrongPassword) {
                const bool encrypted = reader->hasEncryptedItems();
                if (!encrypted) {
                    // Plain archive (or RAR/7z without encryption): no password.
                    return doExtract(*reader);
                }
                if (havePassword) {
                    ExtractResult r = doExtract(*reader);
                    if (r.status != ExtractStatus::NeedPassword) {
                        return r;  // success or a genuine (non-password) error
                    }
                    // Wrong password surfaced at decrypt time: drop the partial
                    // output and re-prompt.
                    clearStaging();
                }
                // else: encrypted but no password yet -> prompt below.
            }

            // --- Solicit a password (first prompt or wrong-password retry) ---
            if (!callbacks.requestPassword) {
                return Fail7z(ExtractStatus::NeedPassword,
                              L"archive is encrypted; a password is required");
            }
            if (attempt >= kMaxPasswordAttempts) {
                return Fail7z(ExtractStatus::NeedPassword,
                              L"incorrect password (retry cap reached)");
            }
            PasswordPrompt prompt;
            prompt.archiveName = archiveName;
            prompt.attempt = attempt;
            std::optional<std::wstring> pw = callbacks.requestPassword(prompt);
            ++attempt;
            if (!pw) {
                return Fail7z(ExtractStatus::Cancelled,
                              L"cancelled at password prompt");
            }
            if (!password.empty()) {
                SecureZeroMemory(password.data(),
                                 password.size() * sizeof(wchar_t));
            }
            password = *pw;
            if (!pw->empty()) {
                SecureZeroMemory(pw->data(), pw->size() * sizeof(wchar_t));
            }
            havePassword = true;
        }
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
