#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "archive_core/detect.h"

namespace ae {

// --- Result -----------------------------------------------------------------

// Outcome category for an extraction attempt. Mapped to user-facing errors in
// task 06; kept coarse here so the engine stays UI-agnostic.
enum class ExtractStatus {
    Ok,            // completed (possibly with skipped special/link entries)
    Cancelled,     // user cancelled via the cancellation token
    Untrusted,     // a zip-slip / absolute / traversal entry was rejected
    OpenFailed,    // could not open the archive (missing, unreadable)
    CorruptInput,  // libarchive read/decode error (truncated/corrupt)
    WriteFailed,   // a filesystem write/create error (disk full, denied, ...)
    NeedPassword,  // an encrypted entry was hit and no password was supplied
    Unsupported,   // the plan's backend is not libarchive / not handled here
};

// Summary of a finished (or aborted) extraction. `entriesWritten` counts
// regular files + directories materialized; `entriesSkipped` counts special
// and link entries that were intentionally skipped.
struct ExtractResult {
    ExtractStatus status = ExtractStatus::Ok;
    std::uint64_t entriesWritten = 0;
    std::uint64_t entriesSkipped = 0;
    // Set when links (sym/hard) were present in the archive (detected + skipped
    // for now; the copy/skip prompt is task 12).
    bool hadLinks = false;
    // A short diagnostic for logging; never contains archive contents.
    std::wstring message;

    bool ok() const { return status == ExtractStatus::Ok; }
};

// --- Progress ---------------------------------------------------------------

// A throttled progress sample. When `total` is 0 the bar is indeterminate.
struct Progress {
    std::uint64_t done = 0;   // bytes processed so far
    std::uint64_t total = 0;  // expected total bytes, or 0 if unknown
    // The entry currently being written (UTF-16), for an optional label. Empty
    // between entries.
    std::wstring currentEntry;
};

// --- Cancellation -----------------------------------------------------------

// A shared, thread-safe cancellation flag. The UI thread calls Cancel(); the
// worker polls IsCancelled() between entries and during chunked writes.
class CancellationToken {
   public:
    void Cancel() noexcept { flag_.store(true, std::memory_order_relaxed); }
    bool IsCancelled() const noexcept {
        return flag_.load(std::memory_order_relaxed);
    }

   private:
    std::atomic<bool> flag_{false};
};

// --- Callbacks --------------------------------------------------------------

// Callbacks the engine invokes on the WORKER thread. All are optional. The UI
// wiring (PostMessage marshaling) is task 05; keeping these as std::function
// keeps the engine testable headless.
struct ExtractCallbacks {
    // Invoked at a throttled rate (~30-60 ms) with the latest progress sample.
    std::function<void(const Progress&)> onProgress;

    // Cancellation token, polled by the engine. May be null (no cancellation).
    CancellationToken* cancel = nullptr;

    // Password request hook. Returns the passphrase to try, or nullopt for
    // "no password". For task 03 callers pass nothing (or a stub returning
    // nullopt); the real prompt is task 10.
    std::function<std::optional<std::wstring>()> requestPassword;
};

// --- The backend-agnostic extraction interface ------------------------------

// Implemented by each backend (libarchive here; 7z.dll/brotli in tasks 09/11).
// `extractToStaging` decodes the input named by `plan.firstPartPath`/the source
// path into `stagingDir`, which must already exist and be on the destination
// volume. The implementation never touches anything outside `stagingDir`.
class IExtractor {
   public:
    virtual ~IExtractor() = default;

    virtual ExtractResult extractToStaging(const PipelinePlan& plan,
                                           std::wstring_view sourcePath,
                                           std::wstring_view stagingDir,
                                           const ExtractCallbacks& callbacks) = 0;
};

// The libarchive-backed implementation covering zip/7z(plain)/tar/gz/bz2/xz/
// zst/lz4 and the compound tar.* forms (filter auto-applied; tar entries read
// directly in one flow). Single-stream inputs decode to exactly one file named
// by `plan.outputNameHint`.
class LibarchiveExtractor final : public IExtractor {
   public:
    ExtractResult extractToStaging(const PipelinePlan& plan,
                                   std::wstring_view sourcePath,
                                   std::wstring_view stagingDir,
                                   const ExtractCallbacks& callbacks) override;
};

// The bit7z/7z.dll-backed implementation (task 09). Covers the cases libarchive
// cannot: encrypted 7z (AES-256, including header-encrypted) and ALL RAR/RAR5
// (better fidelity than libarchive's clean-room RAR reader), plus the matching
// multi-volume chains. The runtime `7z.dll` is loaded ON DEMAND the first time
// this backend actually runs (the `Bit7zLibrary` is constructed lazily inside
// extractToStaging), so plain-zip/libarchive paths never touch 7z.dll and cold
// start is unaffected. Progress/cancel/password semantics mirror
// LibarchiveExtractor so the UI stays backend-agnostic.
class SevenZipExtractor final : public IExtractor {
   public:
    ExtractResult extractToStaging(const PipelinePlan& plan,
                                   std::wstring_view sourcePath,
                                   std::wstring_view stagingDir,
                                   const ExtractCallbacks& callbacks) override;
};

// --- Backend selection / encryption query ----------------------------------

// Complete the detector's encryption re-route hook (ReroutedForEncryption) by
// performing the real, open-time encryption query and returning the backend the
// engine must actually drive for `sourcePath`.
//
// For a plan already routed to SevenZipDll (RAR, or a content-sniffed encrypted
// 7z) the answer is SevenZipDll with no probe. For a plan tentatively on
// LibArchive whose format is 7z, this opens the archive via libarchive and
// checks archive_read_has_encrypted_entries(); an encrypted 7z re-routes to
// SevenZipDll, everything else stays put (libarchive decrypts zip itself). The
// probe is cheap (open + header peek, no extraction) and never throws.
Backend ResolveBackend(const PipelinePlan& plan, std::wstring_view sourcePath);

// --- Multi-volume completeness ---------------------------------------------

// Verify that every sibling volume of a multi-volume set named by `firstPart`
// is present in its directory. Returns true for non-multi-volume plans (nothing
// to check). On a gap, returns false and sets `missing` to the name of the
// first missing volume (basename only) so the caller can surface a precise,
// non-truncating error. Recognizes the 7z split (.7z.NNN), RAR new-style
// (.partN.rar), and RAR old-style (.rar + .rNN) chains.
bool VerifyMultiVolumeComplete(const PipelinePlan& plan,
                               std::wstring_view firstPart,
                               std::wstring& missing);

// --- Temp-staging utilities -------------------------------------------------

// Create a hidden temp staging directory `<workDir>\.archive-extractor-tmp-<id>`
// on the SAME volume as `workDir`. The `<id>` is derived from a GUID/PID source
// (no nondeterministic RNG that would break test reproducibility). Returns the
// full path on success, or empty on failure. The returned path is NOT \\?\-
// prefixed (callers add the prefix per-op); it is a normal Windows path.
std::wstring CreateStagingDir(std::wstring_view workDir);

// Recursively delete a directory tree (used on completion, cancel, or error).
// Tolerates a missing directory. Uses \\?\ extended paths internally. Returns
// true if the tree is gone afterward.
bool RemoveDirTree(std::wstring_view dir);

// True when both paths resolve to the same volume (same drive / mount root).
// Used to assert staging lands on the destination volume so placement is an
// O(1) rename (task 04). On error, returns false.
bool SameVolume(std::wstring_view a, std::wstring_view b);

// Prefix a path with the \\?\ extended-length form if it is a normal absolute
// path. Idempotent: already-prefixed or UNC paths are handled. Exposed for
// tests and the placement step.
std::wstring ToExtendedPath(std::wstring_view path);

// --- Worker-thread driver ---------------------------------------------------

// Run `extractor.extractToStaging(...)` on a detached-style worker thread and
// invoke `onDone` (on the WORKER thread) with the result. The returned thread
// is joinable; the caller owns it. The UI thread never blocks: progress and
// completion are delivered via the callbacks. This keeps the contract
// callback-based and UI-agnostic (PostMessage marshaling is task 05).
std::thread RunExtractionOnWorker(IExtractor& extractor, const PipelinePlan& plan,
                                  std::wstring sourcePath, std::wstring stagingDir,
                                  ExtractCallbacks callbacks,
                                  std::function<void(ExtractResult)> onDone);

}  // namespace ae
