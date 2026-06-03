#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace ae {

// Emit one diagnostic line. Always written to the debugger via
// OutputDebugString; also echoed to stderr when a console/redirect is attached
// (a no-op otherwise). When the rotating file logger has been initialized
// (InitFileLog) the line is ALSO appended to the on-disk log. A trailing
// newline is appended.
//
// SAFETY: callers must NEVER pass archive contents or passwords. Only format /
// error code / entry path / diagnostics belong in the log (design 06).
void Log(std::wstring_view message);

// --- Rotating file log (diagnostics) ----------------------------------------
//
// Initialize the on-disk diagnostic log. The default location is
// %LOCALAPPDATA%\ArchiveExtractor\log.txt (created on first write). Size-capped:
// when the file would exceed `maxBytes`, it is rotated (log.txt -> log.txt.1,
// replacing any previous .1) and a fresh log.txt is started. Idempotent; safe to
// call once at startup. Returns true if the log path was resolved/created.
//
// Pass an explicit `path` to override the location (used by tests). When `path`
// is empty the default %LOCALAPPDATA% location is used.
bool InitFileLog(std::wstring_view path = std::wstring_view(),
                 std::size_t maxBytes = 256 * 1024);

// The resolved log file path (empty if InitFileLog has not run or failed).
// Exposed for diagnostics and tests.
std::wstring LogFilePath();

// Stop file logging and release the path (subsequent Log() calls go only to the
// debugger / stderr). Mainly for tests so a temp log file can be deleted.
void ShutdownFileLog();

}  // namespace ae
