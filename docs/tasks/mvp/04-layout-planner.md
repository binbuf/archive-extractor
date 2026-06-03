# 04 — Layout planner & atomic placement

## Goal
After staging completes, inspect the staged root and place the result into the
working directory using the smart-layout rule, with auto-rename on collision and
an atomic (rename-based) move. This delivers the macOS-Archive-Utility feel and
the all-or-nothing guarantee.

## Depends on
- [03 — Extraction engine](03-extraction-engine-libarchive.md).

## Design refs
- [03-extraction-behavior.md §Smart layout, §Temp-staging step 4, §Auto-rename](../../design/03-extraction-behavior.md).
- [06-error-handling.md §Empty/degenerate](../../design/06-error-handling.md).
- [07-passwords-edge-cases.md §9, §13, §17](../../design/07-passwords-edge-cases.md) — case-only collisions, macOS cruft as 2 entries, degenerate archives.

## Scope — in
- **Root analysis:** count distinct root-level entries in the staged folder.
- **Layout rule:**
  - exactly **1 entry** (file or folder) → place that entry directly into the
    working dir (no extra wrapper; never `project/project/`);
  - **2+ entries** → wrap in a folder named after the **archive stem** and place
    everything inside.
  - Single-stream outputs always take the 1-entry path with the inner filename.
- **Placement via rename:** prefer `MoveFileEx` rename (O(1), same-volume):
  - 1 entry → move that entry out of staging into the working dir;
  - 2+ entries → rename the temp folder itself to the stem, **or** create the
    stem folder and move contents.
- **Auto-rename on collision** (never overwrite): probe incrementally
  `name → name (1) → name (2) → …`. For files insert the counter **before the
  final extension** (`report.pdf` → `report (1).pdf`); for folders append at the
  end. Rename only the **top-level placed item** — inner contents are never
  individually renamed at this stage.
- **Degenerate cases:** empty archive (0 entries) → no output, **no empty wrapper
  folder**, complete cleanly (document the chosen "brief notice vs silent"
  behavior); single empty folder at root → unwrapped per the 1-entry rule.
- **Cleanup:** remove the (now empty) temp folder after placement; on any error,
  remove temp and leave the working dir untouched.
- Return the **placed top-level item path** for the Explorer reveal (task 07).

## Scope — out
- Inner-entry name sanitization (lives in the engine / task 12); the planner only
  renames the single top-level placed item.
- The reveal itself (task 07).

## Deliverables
- A `LayoutPlanner` that takes `(stagingDir, stem, kind)` and returns the final
  placed top-level path (or "no output" for empty).
- Auto-rename utility (file vs folder counter placement).
- Unit/integration tests over the generated corpus (see
  [Testing conventions](README.md#testing-conventions)) covering: single file
  (`*__single-file`), single folder with no double-wrap (`*__single-folder`),
  2+ mixed entries wrapped (`*__multi-files`, `*__multi-folders`, `*__mixed`),
  name collisions for files and folders (extract twice into the same dir),
  case-only collision and duplicate path (`edge-duplicate-path.zip`), `__MACOSX`
  + one real folder → 2 entries → wrapped (`edge-macosx.zip`), empty archive
  (`edge-empty.zip` / `edge-empty.tar`), single empty folder
  (`edge-single-empty-folder.zip`). Every test extracts/places into a unique temp
  dir and **deletes it immediately afterward on every path** (including failure).

## Acceptance criteria
- All worked examples in `03-extraction-behavior.md` produce the documented output.
- Placement is a rename (verified same-volume), not a copy.
- Collisions never overwrite; counters are placed correctly for files vs folders.
- Empty archive leaves no wrapper folder; temp is always cleaned up.
- The returned top-level path is correct and unambiguous (single item).

## Notes & risks
- "Distinct root entries" must be computed from the staged tree's top level only.
- Keep placement transactional: do all collision probing before the move so a
  failure leaves the working dir clean.
