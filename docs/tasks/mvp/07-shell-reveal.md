# 07 — Shell reveal (open-and-select in Explorer)

## Goal
On success, reveal the newly placed top-level item in File Explorer with it
pre-selected — reusing an already-open window on the target folder when possible,
otherwise opening one — and bring it to the front.

## Depends on
- [04 — Layout planner](04-layout-planner.md) (provides the placed top-level path).
- [05 — UI extraction dialog](05-ui-extraction-dialog.md) (reveal hook on completion).

## Design refs
- [04-shell-integration.md §Reveal in Explorer](../../design/04-shell-integration.md).
- [01-architecture.md §Shell Integration](../../design/01-architecture.md).

## Scope — in
- After successful placement, given the **placed top-level item** (single file,
  unwrapped folder, or wrapper folder):
  1. `CoInitializeEx` (already done at startup; ensure availability on this path).
  2. **Reuse path (preferred):** enumerate open Explorer windows via
     `IShellWindows`; if one already shows the **target working directory**,
     reuse it — bring it to the foreground and select the new item there.
  3. **Fallback:** `SHOpenFolderAndSelectItems` with the working-dir PIDL and the
     new item as the selection (opens or focuses a window with the item selected).
  4. `SetForegroundWindow` to bring the window to front.
- Select exactly **one** top-level item (layout guarantees a single unambiguous
  target).
- No reveal on cancel or error.

## Scope — out
- Registration (task 08) and the installer (task 13).

## Deliverables
- A `ShellReveal` component implementing the reuse-then-fallback logic.
- Integration into the success path of the orchestration (replacing the task-05
  stub).
- Manual test matrix: (a) no Explorer open, (b) Explorer already showing the
  target folder, (c) Explorer showing a different folder, (d) multiple Explorer
  windows — verify reuse vs new-window and that the right item is selected.

## Acceptance criteria
- On success the placed item is revealed and pre-selected.
- When a window already shows the target folder, it is reused and brought to
  front rather than opening a duplicate.
- Otherwise a window opens (or focuses) with the item selected.
- No reveal occurs on cancel/error.

## Notes & risks
- `SHOpenFolderAndSelectItems` alone tends to open a new window; the
  `IShellWindows` enumeration is what enables genuine reuse — treat it as the
  primary path and the API as fallback.
- Free PIDLs and COM interfaces correctly; this runs right before process exit.
