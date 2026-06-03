#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ae {

// --- Inner-name sanitization (task 12, design 07 §9) ------------------------
//
// Pure, Win32-free helpers that make archive entry names safe for NTFS and
// resolve case-only collisions / exact duplicates deterministically. Used by the
// extraction engine while staging; the top-level auto-rename (task 04) still
// governs the placed result. Kept here so it is unit-testable without the engine.

// True if `name` is a Windows reserved device name — CON, PRN, AUX, NUL,
// COM1..COM9, LPT1..LPT9, plus CONIN$/CONOUT$ — comparing the base token before
// the first dot, case-insensitively (so "CON", "con.txt", "Aux" all match).
bool IsReservedDeviceName(std::wstring_view name);

// Sanitize ONE path segment (no separators) for NTFS:
//   * a reserved device name gets a trailing '_' on its base ("CON.txt" ->
//     "CON_.txt", "AUX" -> "AUX_");
//   * invalid characters (<>:"|?* and control chars 0x00..0x1F) -> '_';
//   * trailing dots and spaces are stripped (illegal on NTFS).
// Never returns empty: a segment that sanitizes away (e.g. "..." or " ")
// collapses to "_". Pure.
std::wstring SanitizeInnerSegment(std::wstring_view seg);

// Tracks the backslash-separated relative paths chosen so far in ONE extraction
// so that, on case-insensitive NTFS, a case-only collision (README vs readme) is
// auto-renamed and an exact duplicate path (same entry twice) is last-wins. The
// sequence of names produced is deterministic (insertion order + a stable " (n)"
// probe), so repeat extractions of the same archive rename identically.
class InnerNameRegistry {
   public:
    enum class Disposition {
        Fresh,      // first time this path (case-insensitively) is seen
        Duplicate,  // exact same path already used -> caller overwrites (last-wins)
        Renamed,    // case-only collision -> the returned leaf gained a " (n)" suffix
        Merged,     // directory whose case-folded path already exists -> reuse it
    };

    // Resolve `relPath` (already segment-sanitized, backslash-separated, no
    // leading/trailing separator). Directories that case-fold to an existing
    // path are Merged (reuse the canonical path so children land together).
    // Files: an exact repeat is Duplicate; a case-only collision is Renamed with
    // a non-colliding " (n)" leaf. Returns the final relative path to use.
    std::wstring Resolve(std::wstring_view relPath, bool isDir, Disposition& disp);

   private:
    // lowercased rel path -> the exact-case rel path actually used.
    std::unordered_map<std::wstring, std::wstring> used_;
};

// --- Decompression-bomb sanity guard (design 07 §15) ------------------------

struct BombVerdict {
    bool tripped = false;    // true if either condition below fired
    bool ratioTrip = false;  // expansion ratio is absurd (and past the floor)
    bool spaceTrip = false;  // output is within the safety margin of free space
};

// Minimum bytes written before the ratio check can fire, so small but highly
// compressible files never trip. (256 MiB.)
inline constexpr std::uint64_t kBombMinOutputFloor = 256ull * 1024 * 1024;
// Expansion ratio considered "absurd". Legitimate text tops out around 100:1;
// real zip bombs are 1e6:1 and up. 1000:1 is a safe dividing line.
inline constexpr std::uint64_t kBombRatioThreshold = 1000;
// Stop-short margin: warn once the staged output comes within this of filling
// the destination volume, pre-empting the disk-full backstop. (64 MiB.)
inline constexpr std::uint64_t kBombFreeMargin = 64ull * 1024 * 1024;

// Evaluate whether an in-progress extraction looks like a decompression bomb.
//   compressedSize    — size of the source archive on disk (denominator).
//   uncompressedSoFar — uncompressed bytes written so far.
//   freeBytes         — free space on the destination volume (0 = unknown/skip).
// Pure; unit-tested directly. Trips on an absurd ratio past the output floor, or
// when the output approaches free space. The engine WARNS + logs on a trip and
// (absent an explicit decline) continues, with disk-full as the final backstop.
BombVerdict EvaluateBombGuard(std::uint64_t compressedSize,
                             std::uint64_t uncompressedSoFar,
                             std::uint64_t freeBytes);

}  // namespace ae
