#include "archive_core/paths.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace ae {
namespace {

std::wstring ToLower(std::wstring_view s) {
    std::wstring out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return out;
}

}  // namespace

std::wstring ParentDirectory(std::wstring_view path) {
    fs::path p{std::wstring(path)};
    return p.parent_path().wstring();
}

bool PathExists(std::wstring_view path) {
    std::error_code ec;
    return fs::exists(fs::path{std::wstring(path)}, ec);
}

bool HasSupportedExtension(std::wstring_view path) {
    // Final-token extensions. Compound forms (tar.gz/tar.xz/...) match on their
    // trailing token; the t* entries cover the common single-token shorthands.
    static constexpr std::array<std::wstring_view, 17> kExts = {
        L"zip", L"7z", L"zst", L"rar", L"xz", L"gz",   L"lz4", L"tar", L"bz2",
        L"br",  L"tgz", L"tbz", L"tbz2", L"txz", L"tzst", L"tlz4", L"tlz",
    };

    const fs::path p{std::wstring(path)};
    const std::wstring fname = ToLower(p.filename().wstring());
    const auto dot = fname.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= fname.size()) return false;

    std::wstring_view token{fname};
    token.remove_prefix(dot + 1);
    return std::find(kExts.begin(), kExts.end(), token) != kExts.end();
}

}  // namespace ae
