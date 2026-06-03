#include "archive_core/sanitize.h"

#include <algorithm>
#include <array>
#include <cwctype>

namespace ae {
namespace {

std::wstring ToLower(std::wstring_view s) {
    std::wstring out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return out;
}

// The base token of a segment: everything before the first '.' (reserved-name
// checks apply to "CON" in "CON.txt"). A leading dot yields an empty base.
std::wstring_view BaseToken(std::wstring_view seg) {
    const std::size_t dot = seg.find(L'.');
    return dot == std::wstring_view::npos ? seg : seg.substr(0, dot);
}

// Split a leaf into (stem, ext) at the LAST dot, Explorer-style (a leading dot
// is treated as no extension). `ext` includes the dot or is empty.
void SplitLeafExt(std::wstring_view leaf, std::wstring& stem, std::wstring& ext) {
    const std::size_t dot = leaf.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0) {
        stem.assign(leaf);
        ext.clear();
    } else {
        stem.assign(leaf.substr(0, dot));
        ext.assign(leaf.substr(dot));
    }
}

}  // namespace

bool IsReservedDeviceName(std::wstring_view name) {
    const std::wstring base = ToLower(BaseToken(name));
    if (base.empty()) return false;

    static const std::array<std::wstring_view, 6> kSimple = {
        L"con", L"prn", L"aux", L"nul", L"conin$", L"conout$"};
    for (const auto& r : kSimple) {
        if (base == r) return true;
    }
    // COM1..COM9 and LPT1..LPT9 (a single 1-9 digit; COM0/LPT0 are not reserved).
    if (base.size() == 4 && (base.rfind(L"com", 0) == 0 || base.rfind(L"lpt", 0) == 0) &&
        base[3] >= L'1' && base[3] <= L'9') {
        return true;
    }
    return false;
}

std::wstring SanitizeInnerSegment(std::wstring_view seg) {
    std::wstring out;
    out.reserve(seg.size());
    for (wchar_t c : seg) {
        // Invalid NTFS filename characters and control chars -> '_'. (Separators
        // never reach here: the caller has already split on them.)
        if (c < 0x20 || c == L'<' || c == L'>' || c == L':' || c == L'"' ||
            c == L'|' || c == L'?' || c == L'*' || c == L'/' || c == L'\\') {
            out.push_back(L'_');
        } else {
            out.push_back(c);
        }
    }

    // Strip trailing dots and spaces (illegal on NTFS / silently dropped).
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) {
        out.pop_back();
    }

    if (out.empty()) return L"_";

    // Reserved device name -> suffix the base token with '_'.
    if (IsReservedDeviceName(out)) {
        const std::size_t dot = out.find(L'.');
        if (dot == std::wstring::npos) {
            out.push_back(L'_');
        } else {
            out.insert(out.begin() + dot, L'_');
        }
    }
    return out;
}

std::wstring InnerNameRegistry::Resolve(std::wstring_view relPath, bool isDir,
                                        Disposition& disp) {
    const std::wstring rel(relPath);
    const std::wstring key = ToLower(rel);

    auto it = used_.find(key);
    if (it == used_.end()) {
        used_.emplace(key, rel);
        disp = Disposition::Fresh;
        return rel;
    }

    if (isDir) {
        // A directory that case-folds to an existing path: reuse the canonical
        // path so children merge into one folder (NTFS is case-insensitive).
        disp = Disposition::Merged;
        return it->second;
    }

    if (it->second == rel) {
        // Exact same path again: last-wins (the caller overwrites in place).
        disp = Disposition::Duplicate;
        return rel;
    }

    // Case-only collision (e.g. README vs readme): auto-rename the newcomer with
    // a non-colliding " (n)" leaf suffix. Probe deterministically.
    const std::size_t sep = rel.find_last_of(L'\\');
    const std::wstring parent = (sep == std::wstring::npos) ? std::wstring()
                                                            : rel.substr(0, sep);
    const std::wstring leaf =
        (sep == std::wstring::npos) ? rel : rel.substr(sep + 1);
    std::wstring stem, ext;
    SplitLeafExt(leaf, stem, ext);

    for (unsigned n = 1; n != 0; ++n) {
        std::wstring candLeaf = stem + L" (" + std::to_wstring(n) + L")" + ext;
        std::wstring cand = parent.empty() ? candLeaf : parent + L"\\" + candLeaf;
        std::wstring candKey = ToLower(cand);
        if (used_.find(candKey) == used_.end()) {
            used_.emplace(candKey, cand);
            disp = Disposition::Renamed;
            return cand;
        }
    }
    // Unreachable in practice; fall back to last-wins to avoid an infinite loop.
    disp = Disposition::Duplicate;
    return rel;
}

BombVerdict EvaluateBombGuard(std::uint64_t compressedSize,
                             std::uint64_t uncompressedSoFar,
                             std::uint64_t freeBytes) {
    BombVerdict v;
    if (freeBytes > 0 && uncompressedSoFar + kBombFreeMargin >= freeBytes) {
        v.spaceTrip = true;
    }
    if (compressedSize > 0 && uncompressedSoFar >= kBombMinOutputFloor &&
        uncompressedSoFar / compressedSize >= kBombRatioThreshold) {
        v.ratioTrip = true;
    }
    v.tripped = v.ratioTrip || v.spaceTrip;
    return v;
}

}  // namespace ae
