# 05 — UI: extraction progress dialog

## Goal
Build the primary Win32 progress dialog and wire it to the worker-thread engine
so a double-click (or CLI launch with a path) shows live progress and a working
Cancel. After this task, the libarchive formats extract end-to-end with UI.

## Depends on
- [04 — Layout planner](04-layout-planner.md).

## Design refs
- [05-ui-spec.md §Extraction dialog, §General UI](../../design/05-ui-spec.md).
- [01-architecture.md §UI Layer, §Threading](../../design/01-architecture.md).
- [03-extraction-behavior.md §Progress, §Cancellation](../../design/03-extraction-behavior.md).

## Scope — in
- A **small, centered, modeless** top-level window (no minimize/maximize), DPI-v2
  aware, native-themed via the comctl6 manifest.
  - **Label:** `Expanding "<filename>"` using the original archive filename
    (with extension); ellipsis-truncate when long.
  - **Progress bar:** determinate when total size is known, marquee otherwise.
  - **Cancel button** to the right of the bar.
- **Threading model:** UI thread runs the message pump; the worker (task 03)
  posts progress/completion to the UI via `PostMessage`. The UI never blocks on
  extraction.
- **Cancel / Esc / window-close (X)** all trigger cancellation: set the atomic
  flag, let the worker abort, delete temp, close the window. No reveal on cancel.
- **Flash suppression:** for extractions finishing in < ~200 ms, suppress or
  delay showing the window to avoid a flicker. Document the chosen threshold.
- **Completion:** on success the dialog closes; Explorer reveal runs (wired in
  task 07 — a stub hook here). No success dialog.
- **Centering** on the active monitor (primary, or the monitor under the cursor).
- Orchestrate the full flow: launch → detect (02) → extract to staging (03) →
  place (04) → (reveal hook) → exit.

## Scope — out
- The password prompt (task 10) and error dialog (task 06) — define hooks/stubs.
- The actual reveal (task 07).

## Deliverables
- The extraction dialog window class + message handling.
- Worker↔UI `PostMessage` progress/completion protocol.
- The end-to-end orchestration tying detection, engine, and layout together
  behind the dialog.
- Manual test notes / a small harness covering: large archive (determinate bar),
  unknown-size stream (marquee), tiny archive (flash suppression), Cancel at
  various points.

## Acceptance criteria
- Double-clicking (or `ArchiveExtractor.exe <path>`) a libarchive-supported
  archive shows the dialog, advances the bar, and on success closes and (via the
  stub) would reveal the placed item.
- The UI stays responsive throughout (drag the window during a large extract).
- Cancel / Esc / X abort promptly, remove temp, and leave the working dir clean.
- Tiny archives don't produce a visible flicker.
- Crisp rendering at 150%/200% scaling on a secondary monitor.

## Notes & risks
- Keep all extraction logic off the UI thread; the dialog only renders state.
- Marquee vs determinate decision comes from the engine's progress capability,
  not the dialog.
