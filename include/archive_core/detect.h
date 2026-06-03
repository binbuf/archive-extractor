#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ae {

// The concrete container/stream format identified for an input. This is the
// logical format the engine must understand, independent of any wrapping
// compression filter (those live in PipelinePlan::filters).
enum class Format {
    Unknown,  // unrecognized content + extension -> unsupported
    Zip,
    SevenZip,
    Rar,
    Tar,
    Gzip,   // single-stream gzip (bare .gz)
    Bzip2,  // single-stream bzip2 (bare .bz2)
    Xz,     // single-stream xz (bare .xz)
    Zstd,   // single-stream zstd (bare .zst)
    Lz4,    // single-stream lz4 (bare .lz4)
    Brotli,  // single-stream brotli (bare .br) — extension-only detection
};

// A compression filter applied ahead of (or instead of) a container. For
// compound forms (tar.gz, tar.br, ...) the filter decompresses to a tar stream
// that libarchive (or our brotli helper) then untars; for bare single-stream
// inputs the filter alone produces the single output file.
enum class Filter {
    Gzip,
    Bzip2,
    Xz,
    Zstd,
    Lz4,
    Brotli,
};

// Which extraction backend the engine must drive. See design 02-format-support
// "Engine routing & the encryption split".
enum class Backend {
    None,        // Unknown/unsupported — nothing to route
    LibArchive,  // primary: non-encrypted formats, plain 7z, tar.*, encrypted zip
    SevenZipDll,  // bit7z/7z.dll: encrypted 7z and all RAR/RAR5
    Brotli,      // Google brotli library: .br
};

// Single-stream (always exactly one entry, placed directly) vs. container
// (1..N root entries, subject to the smart-layout rules).
enum class Kind {
    Unknown,
    SingleStream,
    Container,
};

// Recognized multi-volume / split-archive naming families (design
// 07-passwords-edge-cases §2). NotMultiVolume covers ordinary single-file
// archives.
enum class VolumeFamily {
    NotMultiVolume,
    SevenZip,    // name.7z.001, .002, ...
    RarPart,     // name.part1.rar, name.part2.rar, ... (RAR5 / new style)
    RarOld,      // name.rar, name.r00, name.r01, ... (old style)
    ZipSpanned,  // name.zip + name.z01, name.z02, ...
};

// The full extraction plan the detector produces from a path (+ optional header
// bytes). Pure data; no Win32 / disk dependencies.
struct PipelinePlan {
    Format format = Format::Unknown;
    Backend backend = Backend::None;
    // Outermost-first list of decompression filters. Empty for plain
    // containers (zip/7z/rar/tar) and for Unknown. For a compound tar.* it
    // holds the single wrapping filter; for a bare single-stream it also holds
    // the single filter and `format` names the matching stream format.
    std::vector<Filter> filters;
    Kind kind = Kind::Unknown;

    // Archive stem: the input filename with all archive/compression extensions
    // and any multi-volume suffix stripped (project.tar.gz -> "project",
    // name.7z.001 -> "name").
    std::wstring stem;

    // For single-stream inputs, the suggested output filename: the inner file
    // name (page.html.br -> "page.html"), falling back to the stem when
    // stripping the compression extension leaves nothing. Empty for containers.
    std::wstring outputNameHint;

    VolumeFamily volumeFamily = VolumeFamily::NotMultiVolume;
    // For a multi-volume set, the path of the first part to open (resolved in
    // the same directory as the input). Equals the input path when the input is
    // already the first part. Empty when not multi-volume.
    std::wstring firstPartPath;

    // True when the content sniff disagreed with the extension and the content
    // won (e.g. a .zip that is really 7z). Drives the "renamed archive" log.
    bool extensionOverriddenByContent = false;

    bool supported() const { return format != Format::Unknown; }
};

// --- Pure helpers (no disk I/O) -------------------------------------------

// Identify a content format purely from a header window (no extension). Returns
// nullopt when the bytes match no known signature. `header` should be the first
// bytes of the file (>= 262 bytes lets the tar "ustar" probe at offset 257
// fire; shorter windows simply skip that probe). Never recognizes brotli (no
// magic).
std::optional<Format> SniffMagic(const std::uint8_t* header, std::size_t len);

// Convenience overload over a byte span-like view.
std::optional<Format> SniffMagic(std::string_view header);

// Compute the plan from a filename plus an already-read header window. This is
// the unit-testable core: callers that have bytes (from disk or a fixture) use
// this directly. `header` may be empty, in which case detection falls back to
// the extension alone.
PipelinePlan PlanFromHeader(std::wstring_view path,
                            const std::uint8_t* header,
                            std::size_t len);

PipelinePlan PlanFromHeader(std::wstring_view path, std::string_view header);

// The number of header bytes worth reading for detection: the first 512 plus
// enough to cover the offset-257 tar probe. A thin disk helper reads this much.
inline constexpr std::size_t kHeaderWindow = 512;

// --- Encryption re-route hook ----------------------------------------------

// The routing rule encodes the encryption split (design 02-format-support): the
// detector picks a backend assuming the common (non-encrypted, clear-header)
// case, then the engine queries real encryption state at open time and calls
// this to re-route if needed. Given a base plan and the now-known encryption
// state, return the backend that must actually handle the input:
//   - encrypted 7z  -> SevenZipDll (libarchive cannot decrypt 7z)
//   - encrypted zip  -> still LibArchive (it decrypts zip)
//   - RAR (any)      -> already SevenZipDll
//   - everything else -> unchanged
// Pure logic; the actual encryption query lives in the engine (task 03/09).
Backend ReroutedForEncryption(const PipelinePlan& plan, bool encrypted);

// --- Disk-backed entry point ----------------------------------------------

// Read a header window from `path` and produce the plan. Thin wrapper over
// PlanFromHeader that performs the only file I/O in this module. On a read
// failure it falls back to extension-only detection.
PipelinePlan DetectFile(std::wstring_view path);

}  // namespace ae
