# 06 — Error handling, error dialog & logging

## Goal
Make every failure **fail loud, fail clean**: a clear error dialog, the working
directory left exactly as found (temp removed), and a diagnostic log. Replace the
placeholder MessageBoxes from earlier tasks with the real catalog.

## Depends on
- [05 — UI extraction dialog](05-ui-extraction-dialog.md).

## Design refs
- [06-error-handling.md](../../design/06-error-handling.md) — full failure-mode table, path safety, long paths, logging.
- [05-ui-spec.md §Error dialog](../../design/05-ui-spec.md).
- [07-passwords-edge-cases.md §14](../../design/07-passwords-edge-cases.md) — source-location quirks (AV lock retry, etc.).

## Scope — in
- A unified **error model** (error code + user-facing message + archive name)
  flowing from engine/layout up to the UI.
- The **error dialog**: title `Archive Extractor`, plain-language body naming the
  archive (e.g. *"Couldn't expand 'broken.zip'. The archive appears to be
  corrupt."*), single **OK** that exits. No partial output left behind. Themed
  task dialog or MessageBox.
- Wire the **failure-mode catalog** to specific messages:
  - unsupported/unknown format (file untouched),
  - corrupt/truncated archive (CRC/read error),
  - encryption not supported by engine (exotic cipher),
  - disk full mid-extract (`ERROR_DISK_FULL`),
  - no write permission in working dir (`ERROR_ACCESS_DENIED`),
  - path too long (mitigated by `\\?\`; else error),
  - file in use / locked on move → **retry with brief backoff**, then error,
  - zip-slip / path-traversal entry → surfaced as untrusted-archive error,
  - archive missing/deleted between launch and read (file not found),
  - cancel (abort, remove temp, no reveal — not an error dialog).
- **Logging:** rotating log at `%LOCALAPPDATA%\ArchiveExtractor\log.txt` recording
  format, error code, and the failing entry. **Never** log archive contents or
  passwords.
- Guarantee temp-staging removal on every error path (defensive cleanup even if a
  later task's backend throws).

## Scope — out
- Password retry messaging (task 10), symlink prompt (task 12) — those tasks add
  their own dialogs; this task owns the generic error dialog + log.

## Deliverables
- Error code enum + message mapping table.
- The error dialog implementation, replacing earlier placeholders.
- Rotating logger (size-capped, contents/password-safe).
- Retry-with-backoff helper for locked-file moves.
- Tests using the generated corpus (see
  [Testing conventions](README.md#testing-conventions)): corrupt input
  (`edge-truncated.zip`), unsupported/unknown format (`edge-unknown.bin`),
  zip-slip (`edge-zip-slip.zip`), plus mocked access-denied dir, disk-full, and
  missing file → each yields the right message and clean cleanup; assert the log
  never contains payload bytes. Tests extract into a unique temp dir and **delete
  it immediately afterward on every path**, and verify the working dir is left
  exactly as found (no partial decompressed output).

## Acceptance criteria
- Each catalog condition produces the documented message and exit, with the
  working dir unchanged and temp removed.
- The log captures format/code/entry but never contents or passwords.
- A transiently locked destination file is retried before erroring.
- Cancel never shows an error dialog.

## Notes & risks
- Centralize cleanup so no future backend can leave litter.
- Keep messages plain-language and consistent with the spec wording.
