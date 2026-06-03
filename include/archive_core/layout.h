#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "archive_core/detect.h"

namespace ae {

// --- Layout planning & atomic placement -------------------------------------
//
// After the engine (task 03) has decoded an archive into a hidden temp staging
// directory on the destination volume, the LayoutPlanner inspects the staged
// root and places the result into the working directory using the macOS
// Archive-Utility "smart layout" rule, with auto-rename on collision and an
// atomic (rename-based) move. See design 03-extraction-behavior.md.

// Coarse outcome of a placement attempt.
enum class PlaceStatus {
    Ok,           // placed; `placedPath` names the top-level item produced
    NoOutput,     // empty archive: nothing was placed (no wrapper created)
    PlaceFailed,  // a filesystem rename/create error; working dir left clean
    BadInput,     // staging dir missing / unreadable / empty arguments
};

// Result of LayoutPlanner::place. On Ok, `placedPath` is the absolute path of
// the single top-level item dropped into the working directory (a file or a
// folder) — exactly what task 07 reveals in Explorer. On NoOutput it is empty.
struct PlaceResult {
    PlaceStatus status = PlaceStatus::PlaceFailed;
    std::wstring placedPath;  // top-level placed item (empty unless Ok)
    // True when the chosen name collided and a "(n)" counter was applied.
    bool renamed = false;
    // Number of distinct root-level entries observed in the staging dir.
    std::size_t rootEntryCount = 0;
    std::wstring message;  // short diagnostic for logging; never archive bytes

    bool ok() const { return status == PlaceStatus::Ok; }
};

// Plans the smart layout and performs the atomic placement.
//
//  - `stagingDir`: the hidden temp dir the engine extracted into. MUST be on the
//    same volume as `workDir` so placement is an O(1) MoveFileEx rename.
//  - `workDir`: the destination directory (normally the archive's folder) the
//    final item is placed into.
//  - `stem`: the archive stem (PipelinePlan::stem) — the wrapper-folder name for
//    the 2+ entry case.
//  - `kind`: SingleStream forces the 1-entry path (using the inner file name);
//    Container applies the count-based rule.
//
// Layout rule:
//   * exactly 1 root entry (file or folder) -> place it directly into workDir
//     (no wrapper; never <stem>/<stem>/). The placed item keeps its OWN name.
//   * 2+ root entries -> wrap them in a folder named `stem` inside workDir.
//   * Single-stream outputs always take the 1-entry path (one decoded file).
//
// Empty archive (0 root entries): NoOutput — nothing is placed and NO empty
// wrapper folder is created. We complete SILENTLY (no notice); the natural
// choice per design 06-error-handling §Empty/degenerate, since there is simply
// nothing to reveal. (The UI layer may surface this however it likes.)
//
// Auto-rename on collision: never overwrite. The target name is probed
// incrementally `name -> name (1) -> name (2) -> ...`. For FILES the counter is
// inserted before the final extension (`report.pdf` -> `report (1).pdf`); for
// FOLDERS it is appended at the end (`project` -> `project (1)`). Only the
// single top-level placed item is renamed — inner contents are never touched.
//
// Transactional: all collision probing happens BEFORE the move, so a failure
// leaves the working directory untouched. The (now-empty) staging dir is
// removed on success; on error it is removed too and `workDir` is left clean.
class LayoutPlanner {
   public:
    PlaceResult place(std::wstring_view stagingDir, std::wstring_view workDir,
                      std::wstring_view stem, Kind kind) const;
};

// --- Auto-rename utility (exposed for unit tests) ---------------------------

// Given a destination directory and a desired base `name`, return a name that
// does not currently exist in `dir`, probing `name -> name (1) -> name (2) ...`.
// When `isFile` is true the counter is inserted before the final extension
// (so `a.tar.gz` becomes `a.tar (1).gz` — only the LAST extension is honored,
// matching Windows Explorer); when false it is appended to the whole name.
// Returns just the leaf name (not a full path). If `name` itself is free it is
// returned unchanged.
std::wstring MakeNonCollidingName(std::wstring_view dir, std::wstring_view name,
                                  bool isFile);

}  // namespace ae
