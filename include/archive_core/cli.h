#pragma once

#include <string>
#include <vector>

namespace ae {

// Launch modes derived from the command line (see design 01-architecture.md).
enum class Mode {
    Extract,     // default: <archive-path>
    Register,    // --register   (shell registration; real logic in task 08)
    Unregister,  // --unregister (shell unregistration; real logic in task 08)
    None,        // no archive given — a no-op, not an error (design §16)
};

struct CommandLine {
    Mode mode = Mode::None;
    std::wstring archivePath;  // populated only for Mode::Extract
    bool showUsage = false;    // --help / -? / /?
};

// Parse the argument vector *excluding* the program name (i.e. argv[1..]).
// Recognizes --register/--unregister (and /-prefixed and case-insensitive
// variants); otherwise the first non-flag token is treated as the archive path.
CommandLine ParseCommandLine(const std::vector<std::wstring>& args);

}  // namespace ae
