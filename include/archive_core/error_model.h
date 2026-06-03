#pragma once

#include <string>
#include <string_view>

#include "archive_core/detect.h"
#include "archive_core/extract.h"
#include "archive_core/layout.h"

namespace ae {

// --- Unified error model ----------------------------------------------------
//
// Task 06: every failure surfaces as a single, UI-agnostic ErrorInfo carrying a
// stable error CODE (for logging + tests), the user-facing MESSAGE (plain
// language, naming the archive), and the archive's leaf name. The engine
// (ExtractStatus), the layout planner (PlaceStatus) and the detector's
// unsupported result are BRIDGED into this catalog; nothing is reinvented.
//
// Pure, headless, GUI-free — fully unit-testable. The Win32 error dialog
// (src/app) consumes ErrorInfo; the rotating logger records code + format +
// failing entry but NEVER archive contents or passwords.

// Stable, exhaustive catalog of failure conditions (design 06 failure-mode
// table). Each maps to exactly one user-facing message family.
enum class ErrorCode {
    None = 0,        // no error (success / cancel — not shown as an error)
    Unsupported,     // unsupported / unknown format; file untouched
    Corrupt,         // corrupt / truncated archive (CRC / read error)
    Encryption,      // an encrypted entry an engine cannot decrypt (exotic cipher)
    NeedPassword,    // an encrypted entry was hit and no password was supplied
    DiskFull,        // out of disk space mid-extract (ERROR_DISK_FULL)
    AccessDenied,    // no write permission in the working dir (ERROR_ACCESS_DENIED)
    PathTooLong,     // a path exceeded limits even with \\?\ (ERROR_*_NAME_*)
    FileInUse,       // destination locked even after retry-with-backoff
    Untrusted,       // zip-slip / path-traversal / absolute entry -> unsafe archive
    FileNotFound,    // archive missing / deleted between launch and read
    WriteFailed,     // generic filesystem write/create failure
    PlaceFailed,     // generic placement (rename/move) failure
    Internal,        // staging-dir creation / unexpected internal failure
};

// A complete, self-contained error description ready for the dialog + log.
struct ErrorInfo {
    ErrorCode code = ErrorCode::None;
    std::wstring archiveName;  // leaf name with extension (e.g. "broken.zip")
    std::wstring message;      // user-facing, plain-language, names the archive

    bool isError() const { return code != ErrorCode::None; }
};

// --- Catalog -----------------------------------------------------------------

// The plain-language message for `code`, naming `archiveName`. This is the
// single source of truth for the wording in the spec / UI spec; the dialog and
// the tests both go through it. `archiveName` should be the leaf name.
std::wstring ErrorMessageFor(ErrorCode code, std::wstring_view archiveName);

// A short, stable, ASCII token for `code` used in the diagnostic log (e.g.
// "CORRUPT"). Never localized; safe to grep.
const wchar_t* ErrorCodeToken(ErrorCode code);

// Build a complete ErrorInfo from a code + archive name (runs the catalog).
ErrorInfo MakeError(ErrorCode code, std::wstring_view archiveName);

// --- Bridges: engine / layout / detector status -> ErrorCode ----------------

// Map an engine ExtractStatus to the catalog. Ok/Cancelled map to None (not
// errors). WriteFailed is refined to DiskFull / AccessDenied when the engine
// supplied a Win32 error in `lastError` (0 = unknown -> generic WriteFailed).
ErrorCode ErrorCodeFromExtract(ExtractStatus status, unsigned long lastError = 0);

// Map a layout PlaceStatus to the catalog. Ok/NoOutput map to None.
// PlaceFailed is refined to DiskFull / AccessDenied / FileInUse / PathTooLong
// when `lastError` carries the originating Win32 code.
ErrorCode ErrorCodeFromPlace(PlaceStatus status, unsigned long lastError = 0);

// Map a detector result: an unsupported plan (no recognized format/backend, or
// a backend this build cannot drive) -> Unsupported; otherwise None.
ErrorCode ErrorCodeFromPlan(const PipelinePlan& plan);

// Refine a generic write/placement failure using a raw Win32 error code (from
// GetLastError). Returns DiskFull / AccessDenied / FileInUse / PathTooLong for
// the known codes, or `fallback` for anything else (or 0).
ErrorCode RefineWin32WriteError(unsigned long lastError, ErrorCode fallback);

}  // namespace ae
