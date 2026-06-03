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
    for (const auto& arg : args) {
        if (IsFlag(arg, L"register")) {
            cl.mode = Mode::Register;
            return cl;
        }
        if (IsFlag(arg, L"unregister")) {
            cl.mode = Mode::Unregister;
            return cl;
        }
        if (IsFlag(arg, L"help") || arg == L"-?" || arg == L"/?") {
            cl.showUsage = true;
            return cl;
        }
        // First non-flag token is the archive path.
        if (!arg.empty() && arg.front() != L'-' && arg.front() != L'/') {
            cl.mode = Mode::Extract;
            cl.archivePath = arg;
            return cl;
        }
    }
    return cl;  // Mode::None
}

}  // namespace ae
