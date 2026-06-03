#include "archive_core/detect.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace ae {
namespace {

std::wstring ToLower(std::wstring_view s) {
    std::wstring out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return out;
}

bool StartsWith(const std::uint8_t* h, std::size_t len, const char* sig,
                std::size_t sigLen) {
    if (len < sigLen) return false;
    return std::memcmp(h, sig, sigLen) == 0;
}

// --- Filename token model --------------------------------------------------

// Decomposed filename: the leading name (everything before the first relevant
// extension token), plus the lowercased trailing extension tokens, outermost
// last. e.g. "Project.Final.tar.gz" -> name="Project.Final", tokens=[tar, gz].
// Only archive/compression/volume-related trailing tokens are peeled; the first
// non-archive token (or any token containing no further dot) terminates peeling.
struct NameParts {
    std::wstring filename;  // basename, original case
    std::wstring lower;     // basename, lowercased
};

NameParts SplitName(std::wstring_view path) {
    NameParts p;
    p.filename = fs::path(std::wstring(path)).filename().wstring();
    p.lower = ToLower(p.filename);
    return p;
}

bool EndsWith(std::wstring_view s, std::wstring_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// True if `s` ends with ".NNN" where NNN are >= 1 decimal digits, returning the
// position of the dot. Used for 7z split (name.7z.001) detection.
bool EndsWithDotDigits(std::wstring_view s, std::size_t& dotPos) {
    if (s.empty() || !iswdigit(s.back())) return false;
    std::size_t i = s.size();
    while (i > 0 && iswdigit(s[i - 1])) --i;
    if (i == 0 || s[i - 1] != L'.') return false;
    dotPos = i - 1;
    return true;
}

// --- Extension classification ---------------------------------------------

struct ExtMatch {
    Format format = Format::Unknown;  // implied container/stream format
    Kind kind = Kind::Unknown;
    // For compound (filter+tar) and bare single-stream forms, the wrapping
    // filter; nullopt for plain containers (zip/7z/rar/tar) and unknown.
    std::optional<Filter> filter;
    bool matched = false;
};

// Classify the trailing extension(s) of a lowercased basename into a plan
// skeleton. Compound forms (tar.gz, tgz, ...) yield filter+tar; bare
// single-stream forms (gz, bz2, ...) yield a single-stream filter; plain
// containers yield no filter. .br is here too (extension-only). Multi-volume
// suffixes are normalized by the caller before this is consulted.
ExtMatch ClassifyExtension(std::wstring_view lower) {
    struct Entry {
        std::wstring_view suffix;  // dotted, lowercased
        Format format;
        Kind kind;
        std::optional<Filter> filter;
    };
    // Order matters: compound forms must be tested before their bare tails so
    // ".tar.gz" beats ".gz".
    static const std::array<Entry, 26> kTable = {{
        // Compound (filter -> tar) forms.
        {L".tar.gz", Format::Tar, Kind::Container, Filter::Gzip},
        {L".tgz", Format::Tar, Kind::Container, Filter::Gzip},
        {L".tar.bz2", Format::Tar, Kind::Container, Filter::Bzip2},
        {L".tbz2", Format::Tar, Kind::Container, Filter::Bzip2},
        {L".tbz", Format::Tar, Kind::Container, Filter::Bzip2},
        {L".tar.xz", Format::Tar, Kind::Container, Filter::Xz},
        {L".txz", Format::Tar, Kind::Container, Filter::Xz},
        {L".tar.zst", Format::Tar, Kind::Container, Filter::Zstd},
        {L".tzst", Format::Tar, Kind::Container, Filter::Zstd},
        {L".tar.lz4", Format::Tar, Kind::Container, Filter::Lz4},
        {L".tlz4", Format::Tar, Kind::Container, Filter::Lz4},
        {L".tar.br", Format::Tar, Kind::Container, Filter::Brotli},
        // Plain containers.
        {L".tar", Format::Tar, Kind::Container, std::nullopt},
        {L".zip", Format::Zip, Kind::Container, std::nullopt},
        {L".7z", Format::SevenZip, Kind::Container, std::nullopt},
        {L".rar", Format::Rar, Kind::Container, std::nullopt},
        // Bare single-stream compressors.
        {L".gz", Format::Gzip, Kind::SingleStream, Filter::Gzip},
        {L".bz2", Format::Bzip2, Kind::SingleStream, Filter::Bzip2},
        {L".xz", Format::Xz, Kind::SingleStream, Filter::Xz},
        {L".zst", Format::Zstd, Kind::SingleStream, Filter::Zstd},
        {L".lz4", Format::Lz4, Kind::SingleStream, Filter::Lz4},
        {L".br", Format::Brotli, Kind::SingleStream, Filter::Brotli},
        // Less-common single-stream shorthands.
        {L".tlz", Format::Tar, Kind::Container, Filter::Xz},  // tar.lzma family
        {L".lz", Format::Xz, Kind::SingleStream, Filter::Xz},
        {L".lzma", Format::Xz, Kind::SingleStream, Filter::Xz},
        {L".bz", Format::Bzip2, Kind::SingleStream, Filter::Bzip2},
    }};

    for (const auto& e : kTable) {
        if (EndsWith(lower, e.suffix)) {
            return {e.format, e.kind, e.filter, true};
        }
    }
    return {};
}

// --- Multi-volume detection ------------------------------------------------

struct VolumeInfo {
    VolumeFamily family = VolumeFamily::NotMultiVolume;
    // The basename of the first part to open, in the original directory's case
    // style as far as we can reconstruct (we build it from the leading name).
    std::wstring firstPartFilename;
    // The portion of the lowercased basename to feed to ClassifyExtension /
    // stem computation as if it were a normal single archive (e.g. for
    // "name.7z.001" this is "name.7z"; for "name.part2.rar" this is "name.rar").
    std::wstring normalizedLower;
    // The stem (volume + archive suffixes stripped).
    std::wstring stem;
    bool detected = false;
};

// Detect the multi-volume family from the lowercased basename. `originalName`
// carries the original-case basename so the reconstructed first-part filename
// keeps the user's casing for the leading portion.
VolumeInfo DetectVolume(std::wstring_view originalName, std::wstring_view lower) {
    VolumeInfo v;

    // 7z split: <base>.7z.<NNN>
    {
        std::size_t dotPos = 0;
        if (EndsWithDotDigits(lower, dotPos)) {
            std::wstring_view head = lower.substr(0, dotPos);
            if (EndsWith(head, L".7z")) {
                v.family = VolumeFamily::SevenZip;
                // Leading name = original up to the ".7z" boundary.
                const std::size_t baseLen = head.size() - 3;  // strip ".7z"
                const std::wstring base(originalName.substr(0, baseLen));
                v.stem = base;
                v.firstPartFilename = base + L".7z.001";
                v.normalizedLower = std::wstring(head);  // "<base>.7z"
                v.detected = true;
                return v;
            }
        }
    }

    // RAR new style: <base>.partN.rar
    {
        // Find ".part<digits>.rar" suffix.
        static constexpr std::wstring_view kRar = L".rar";
        if (EndsWith(lower, kRar)) {
            std::wstring_view stem = lower.substr(0, lower.size() - kRar.size());
            // stem must end with ".part<digits>"
            std::size_t i = stem.size();
            while (i > 0 && iswdigit(stem[i - 1])) --i;
            const bool hasDigits = i < stem.size();
            static constexpr std::wstring_view kPart = L".part";
            if (hasDigits && i >= kPart.size() &&
                stem.compare(i - kPart.size(), kPart.size(), kPart) == 0) {
                const std::size_t baseLen = i - kPart.size();
                const std::wstring base(originalName.substr(0, baseLen));
                v.family = VolumeFamily::RarPart;
                v.stem = base;
                v.firstPartFilename = base + L".part1.rar";
                // normalizedLower reflects a plain ".rar" for routing/stem.
                v.normalizedLower = ToLower(base) + L".rar";
                v.detected = true;
                return v;
            }
        }
    }

    // RAR old style: <base>.r<NN> (continuation parts; .rar is the first part).
    {
        if (lower.size() >= 4 && lower[lower.size() - 4] == L'.' &&
            lower[lower.size() - 3] == L'r' && iswdigit(lower[lower.size() - 2]) &&
            iswdigit(lower[lower.size() - 1])) {
            const std::size_t baseLen = lower.size() - 4;  // strip ".rNN"
            const std::wstring base(originalName.substr(0, baseLen));
            v.family = VolumeFamily::RarOld;
            v.stem = base;
            v.firstPartFilename = base + L".rar";
            v.normalizedLower = ToLower(base) + L".rar";
            v.detected = true;
            return v;
        }
    }

    // zip spanned: <base>.z<NN> (continuation parts; .zip is the first part).
    {
        if (lower.size() >= 4 && lower[lower.size() - 4] == L'.' &&
            lower[lower.size() - 3] == L'z' && iswdigit(lower[lower.size() - 2]) &&
            iswdigit(lower[lower.size() - 1])) {
            const std::size_t baseLen = lower.size() - 4;  // strip ".zNN"
            const std::wstring base(originalName.substr(0, baseLen));
            v.family = VolumeFamily::ZipSpanned;
            v.stem = base;
            v.firstPartFilename = base + L".zip";
            v.normalizedLower = ToLower(base) + L".zip";
            v.detected = true;
            return v;
        }
    }

    return v;  // not multi-volume
}

// --- Stem / output-name computation ----------------------------------------

// Strip the matched extension (and, for compound, the leading ".tar") from a
// basename to produce the stem. `ext` is the ExtMatch the basename classified
// to. Operates on the original-case basename for fidelity.
std::wstring StemFromExtension(std::wstring_view originalName,
                               std::wstring_view lower, const ExtMatch& ext) {
    // Identify how many trailing chars to strip by matching against the same
    // table the classification used (longest matching suffix wins, already
    // ensured by classification order). We re-find the suffix length here.
    // Map of the suffix we matched:
    struct StripEntry {
        std::wstring_view suffix;
    };
    static const std::array<std::wstring_view, 26> kSuffixes = {{
        L".tar.gz", L".tgz", L".tar.bz2", L".tbz2", L".tbz", L".tar.xz",
        L".txz", L".tar.zst", L".tzst", L".tar.lz4", L".tlz4", L".tar.br",
        L".tar", L".zip", L".7z", L".rar", L".gz", L".bz2", L".xz", L".zst",
        L".lz4", L".br", L".tlz", L".lz", L".lzma", L".bz",
    }};
    for (const auto& s : kSuffixes) {
        if (EndsWith(lower, s)) {
            return std::wstring(originalName.substr(0, originalName.size() - s.size()));
        }
    }
    (void)ext;
    return std::wstring(originalName);
}

// Map a content-sniffed Format to its Kind. Sniffing yields concrete stream /
// container formats; tar sniffs as Tar (container), gzip as Gzip (single
// stream), etc.
Kind KindOf(Format f) {
    switch (f) {
        case Format::Zip:
        case Format::SevenZip:
        case Format::Rar:
        case Format::Tar:
            return Kind::Container;
        case Format::Gzip:
        case Format::Bzip2:
        case Format::Xz:
        case Format::Zstd:
        case Format::Lz4:
        case Format::Brotli:
            return Kind::SingleStream;
        case Format::Unknown:
            return Kind::Unknown;
    }
    return Kind::Unknown;
}

// The single-stream filter implied by a bare stream Format, if any.
std::optional<Filter> FilterOf(Format f) {
    switch (f) {
        case Format::Gzip:
            return Filter::Gzip;
        case Format::Bzip2:
            return Filter::Bzip2;
        case Format::Xz:
            return Filter::Xz;
        case Format::Zstd:
            return Filter::Zstd;
        case Format::Lz4:
            return Filter::Lz4;
        case Format::Brotli:
            return Filter::Brotli;
        default:
            return std::nullopt;
    }
}

// The stream Format that a given filter decompresses from (used to decide
// whether a compound-form extension agrees with the content sniff: a .tar.gz
// sniffs as Gzip, which matches its Gzip filter).
Format StreamFormatOfFilter(Filter f) {
    switch (f) {
        case Filter::Gzip:
            return Format::Gzip;
        case Filter::Bzip2:
            return Format::Bzip2;
        case Filter::Xz:
            return Format::Xz;
        case Filter::Zstd:
            return Format::Zstd;
        case Filter::Lz4:
            return Format::Lz4;
        case Filter::Brotli:
            return Format::Brotli;
    }
    return Format::Unknown;
}

// True when a content sniff agrees with a compound (filter->tar) extension:
// the sniffed format equals the wrapping filter's stream format.
bool AgreesWithFilter(Format sniffed, const ExtMatch& ext) {
    if (!ext.filter) return false;
    return sniffed == StreamFormatOfFilter(*ext.filter);
}

// Strip exactly one trailing single-stream compression extension from a
// basename to recover the inner filename (page.html.br -> page.html,
// document.txt.gz -> document.txt). Returns empty if nothing precedes it.
std::wstring StripSingleStreamExt(std::wstring_view originalName,
                                  std::wstring_view lower) {
    static const std::array<std::wstring_view, 8> kSingle = {{
        L".gz", L".bz2", L".xz", L".zst", L".lz4", L".br", L".lz", L".lzma",
    }};
    for (const auto& s : kSingle) {
        if (EndsWith(lower, s)) {
            return std::wstring(
                originalName.substr(0, originalName.size() - s.size()));
        }
    }
    return std::wstring(originalName);
}

Backend RouteBackend(Format f, const std::vector<Filter>& filters) {
    if (f == Format::Unknown) return Backend::None;
    // Any brotli wrapping filter must go through our brotli backend — libarchive
    // cannot decompress brotli. This covers both the bare `.br` single stream
    // (format == Brotli) and the compound `.tar.br` (format == Tar, brotli
    // filter): the latter is brotli-decoded to a temp tar, then untarred.
    for (const Filter fil : filters) {
        if (fil == Filter::Brotli) return Backend::Brotli;
    }
    switch (f) {
        case Format::Unknown:
            return Backend::None;
        case Format::Brotli:
            return Backend::Brotli;
        case Format::Rar:
            // All RAR/RAR5 routed to 7z.dll for fidelity + crypto.
            return Backend::SevenZipDll;
        case Format::Zip:
        case Format::SevenZip:
        case Format::Tar:
        case Format::Gzip:
        case Format::Bzip2:
        case Format::Xz:
        case Format::Zstd:
        case Format::Lz4:
            // Primary engine. (Encrypted 7z re-routes to 7z.dll at engine-open
            // time via ReroutedForEncryption(); the detector encodes the rule.)
            return Backend::LibArchive;
    }
    (void)filters;
    return Backend::None;
}

}  // namespace

std::optional<Format> SniffMagic(const std::uint8_t* h, std::size_t len) {
    if (h == nullptr || len == 0) return std::nullopt;

    if (StartsWith(h, len, "7z\xBC\xAF\x27\x1C", 6)) return Format::SevenZip;
    if (StartsWith(h, len, "PK\x03\x04", 4)) return Format::Zip;
    // Empty/spanned zip markers also begin with "PK".
    if (StartsWith(h, len, "PK\x05\x06", 4)) return Format::Zip;
    if (StartsWith(h, len, "PK\x07\x08", 4)) return Format::Zip;
    if (StartsWith(h, len, "Rar!\x1A\x07", 6)) return Format::Rar;
    if (StartsWith(h, len, "\x1F\x8B", 2)) return Format::Gzip;
    if (StartsWith(h, len, "BZh", 3)) return Format::Bzip2;
    if (StartsWith(h, len, "\xFD" "7zXZ\x00", 6)) return Format::Xz;
    if (StartsWith(h, len, "\x28\xB5\x2F\xFD", 4)) return Format::Zstd;
    if (StartsWith(h, len, "\x04\x22\x4D\x18", 4)) return Format::Lz4;

    // tar: "ustar" magic at offset 257 (POSIX/GNU). No leading magic.
    if (len >= 257 + 5 && std::memcmp(h + 257, "ustar", 5) == 0) {
        return Format::Tar;
    }
    return std::nullopt;
}

std::optional<Format> SniffMagic(std::string_view header) {
    return SniffMagic(reinterpret_cast<const std::uint8_t*>(header.data()),
                      header.size());
}

PipelinePlan PlanFromHeader(std::wstring_view path, const std::uint8_t* header,
                            std::size_t len) {
    PipelinePlan plan;

    const NameParts name = SplitName(path);

    // 1. Multi-volume normalization. If the basename is a volume member, peel
    //    the volume suffix and proceed as if it were the first single archive.
    const VolumeInfo vol = DetectVolume(name.filename, name.lower);
    std::wstring effectiveLower = vol.detected ? vol.normalizedLower : name.lower;
    std::wstring effectiveName =
        vol.detected ? fs::path(vol.firstPartFilename).filename().wstring()
                     : name.filename;
    // For stem/extension purposes on volume members we want the normalized
    // single-archive name (e.g. "<base>.7z"); reconstruct original-case head.
    std::wstring stemBaseOriginal = vol.detected ? vol.stem : std::wstring();

    // 2. Extension classification (on the normalized name).
    const ExtMatch ext = ClassifyExtension(effectiveLower);

    // 3. Content sniff.
    const std::optional<Format> sniffed = SniffMagic(header, len);

    // 4. Reconcile content vs extension.
    //    - .br is content-invisible: extension is authoritative, sniff ignored
    //      for the brotli decision.
    //    - Otherwise prefer content when it disagrees with a recognized
    //      extension, and flag the override. When the extension is unknown,
    //      content alone drives detection.
    Format format = Format::Unknown;
    Kind kind = Kind::Unknown;
    std::vector<Filter> filters;
    bool overridden = false;

    const bool extIsBrotli =
        ext.matched && (ext.format == Format::Brotli ||
                        (ext.filter && *ext.filter == Filter::Brotli));

    if (extIsBrotli) {
        // Extension-only path (bare .br or .tar.br). Never let a sniff change
        // it; brotli streams have no magic anyway.
        format = ext.format;
        kind = ext.kind;
        if (ext.filter) filters.push_back(*ext.filter);
    } else if (sniffed) {
        // Content recognized.
        if (ext.matched) {
            // Both present. Determine whether they agree at the container
            // level. For bare single-stream extensions the sniffed format
            // equals the stream format; for compound forms the sniffed format
            // is the *filter's* stream format (e.g. tar.gz sniffs as Gzip),
            // which still "agrees" with the extension's intent.
            const bool agrees = (*sniffed == ext.format) ||
                                AgreesWithFilter(*sniffed, ext);
            if (agrees) {
                format = ext.format;
                kind = ext.kind;
                if (ext.filter) filters.push_back(*ext.filter);
            } else {
                // Trust content; flag override. The plan becomes whatever the
                // content says (a plain container or a bare single-stream).
                format = *sniffed;
                kind = KindOf(*sniffed);
                if (auto f = FilterOf(*sniffed)) filters.push_back(*f);
                overridden = true;
            }
        } else {
            // Unknown extension, known content: content drives.
            format = *sniffed;
            kind = KindOf(*sniffed);
            if (auto f = FilterOf(*sniffed)) filters.push_back(*f);
        }
    } else if (ext.matched) {
        // No usable content signal (header empty/short or unrecognized) but the
        // extension is known: trust the extension.
        format = ext.format;
        kind = ext.kind;
        if (ext.filter) filters.push_back(*ext.filter);
    }
    // else: both unknown -> remains Format::Unknown (unsupported).

    plan.format = format;
    plan.kind = kind;
    plan.filters = std::move(filters);
    plan.backend = RouteBackend(plan.format, plan.filters);
    plan.extensionOverriddenByContent = overridden;

    // 5. Stem + output-name hint.
    if (vol.detected) {
        plan.stem = vol.stem;
    } else if (ext.matched) {
        plan.stem = StemFromExtension(effectiveName, effectiveLower, ext);
    } else if (format != Format::Unknown) {
        // Content-detected with unknown extension: stem = full basename.
        plan.stem = effectiveName;
    } else {
        plan.stem = effectiveName;
    }

    if (plan.kind == Kind::SingleStream) {
        // Output name = inner filename: strip the trailing compression
        // extension; fall back to the stem when nothing remains.
        std::wstring hint = StripSingleStreamExt(effectiveName, effectiveLower);
        if (hint.empty()) hint = plan.stem;
        plan.outputNameHint = hint;
    }

    // 6. Multi-volume bookkeeping.
    plan.volumeFamily = vol.family;
    if (vol.detected) {
        // Resolve the first part path in the input's directory.
        const std::wstring parent =
            fs::path(std::wstring(path)).parent_path().wstring();
        plan.firstPartPath =
            parent.empty() ? vol.firstPartFilename
                           : (fs::path(parent) / vol.firstPartFilename).wstring();
    }

    (void)stemBaseOriginal;
    return plan;
}

PipelinePlan PlanFromHeader(std::wstring_view path, std::string_view header) {
    return PlanFromHeader(path,
                          reinterpret_cast<const std::uint8_t*>(header.data()),
                          header.size());
}

Backend ReroutedForEncryption(const PipelinePlan& plan, bool encrypted) {
    if (!encrypted) return plan.backend;
    // libarchive can decrypt zip; it cannot decrypt 7z. RAR is already on the
    // 7z.dll backend. Single-stream formats are never encrypted in our matrix.
    if (plan.format == Format::SevenZip) return Backend::SevenZipDll;
    return plan.backend;
}

PipelinePlan DetectFile(std::wstring_view path) {
    std::array<std::uint8_t, 512> buf{};
    std::size_t got = 0;
    std::ifstream in(fs::path(std::wstring(path)), std::ios::binary);
    if (in) {
        in.read(reinterpret_cast<char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
        got = static_cast<std::size_t>(in.gcount());
    }
    return PlanFromHeader(path, buf.data(), got);
}

}  // namespace ae
