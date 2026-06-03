#include "archive_core/cli.h"

#include <cwctype>
#include <string_view>

namespace ae {
namespace {

bool IEquals(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (towlower(a[i]) != towlower(b[i])) return false;
    }
    return true;
}

// Accepts both "--flag" and "/flag" spellings, case-insensitively.
bool IsFlag(std::wstring_view arg, std::wstring_view name) {
    return IEquals(arg, std::wstring(L"--") + std::wstring(name)) ||
           IEquals(arg, std::wstring(L"/") + std::wstring(name));
}

}  // namespace

CommandLine ParseCommandLine(const std::vector<std::wstring>& args) {
    CommandLine cl;
    bool wantSetDefault = false;  // saw --set-default; next non-flag is the file
    for (const auto& arg : args) {
        if (IsFlag(arg, L"register")) {
            cl.mode = Mode::Register;
            return cl;
        }
        if (IsFlag(arg, L"unregister")) {
            cl.mode = Mode::Unregister;
            return cl;
        }
        // --set-default [file]: open the OS "Open with" picker so the USER can
        // make us the default for an owned extension (e.g. .zip). The optional
        // following token is a sample file of the type to set the default for.
        if (IsFlag(arg, L"set-default")) {
            cl.mode = Mode::SetDefault;
            wantSetDefault = true;
            continue;
        }
        if (IsFlag(arg, L"help") || arg == L"-?" || arg == L"/?") {
            cl.showUsage = true;
            return cl;
        }
        // Non-flag token = a path. The first becomes archivePath (the sample
        // file for --set-default, else the archive to extract); subsequent ones
        // in Extract mode collect as extraPaths (one-process-per-path; design
        // §16). For --set-default we only need the one sample file.
        if (!arg.empty() && arg.front() != L'-' && arg.front() != L'/') {
            if (cl.archivePath.empty()) {
                cl.archivePath = arg;
                if (!wantSetDefault) cl.mode = Mode::Extract;
            } else if (cl.mode == Mode::Extract) {
                cl.extraPaths.push_back(arg);
            }
        }
    }
    return cl;  // Mode::None (or SetDefault with no sample file)
}

}  // namespace ae
