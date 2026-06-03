#pragma once

#include <string>
#include <vector>

namespace ae {

// Launch modes derived from the command line (see design 01-architecture.md).
enum class Mode {
    Extract,     // default: <archive-path>
    Register,    // --register   (shell registration; HKLM, elevated)
    Unregister,  // --unregister (shell unregistration; HKLM, elevated)
    SetDefault,  // --set-default <archive-path> (user-driven default picker)
    None,        // no archive given — a no-op, not an error (design §16)
};

struct CommandLine {
    Mode mode = Mode::None;
    // Populated for Mode::Extract (the archive to extract) and for
    // Mode::SetDefault (the sample file whose type the user picks a default for).
    std::wstring archivePath;
    bool showUsage = false;    // --help / -? / /?
};

// Parse the argument vector *excluding* the program name (i.e. argv[1..]).
// Recognizes --register/--unregister (and /-prefixed and case-insensitive
// variants); otherwise the first non-flag token is treated as the archive path.
CommandLine ParseCommandLine(const std::vector<std::wstring>& args);

}  // namespace ae
