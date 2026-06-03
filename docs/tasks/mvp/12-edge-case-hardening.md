# 12 — Edge-case hardening (links, names, source quirks, bombs)

## Goal
Harden the long tail: symlinks/hardlinks, illegal/reserved/duplicate names,
filename encoding corner cases, macOS-authored cruft, source-location quirks,
decompression-bomb sanity, and launch-argument edge cases. These make the tool
robust on real-world archives.

## Depends on
- [03 — Extraction engine](03-extraction-engine-libarchive.md) and
  [04 — Layout planner](04-layout-planner.md) (sanitization + placement hooks).
- [06 — Error handling](06-error-handling.md) (dialog/log infrastructure).

## Design refs
- [07-passwords-edge-cases.md §6, §7, §8, §9, §13, §14, §15, §16, §17](../../design/07-passwords-edge-cases.md).
- [06-error-handling.md §Path safety, §Empty/degenerate](../../design/06-error-handling.md).

## Scope — in
- **Symlinks & hardlinks (tar etc.):** when links are present, show a **one-time
  prompt** for the whole extraction — **[Copy targets]** / **[Skip links]**.
  - Copy targets → replace each link with a copy of its target's contents;
    self-referential/external (`/etc/...`, out-of-archive) targets can't be
    copied → skip + log those individual links.
  - Skip links → omit link entries; log what was skipped.
  - Hardlinks materialized as independent copies (Copy) or skipped (Skip).
  - Do **not** create real Windows symlinks/junctions in v1. Prompt appears at
    most once; choice applies to all links.
- **Special/non-file entries** (device/FIFO/socket): skip + log (confirm task-03
  baseline); empty directories preserved.
- **Filename encoding:** zip UTF-8 flag vs legacy code page (CP437/OEM)
  best-effort heuristic; 7z/RAR decode directly; normalize all to UTF-16.
- **Illegal names, collisions, duplicates (inner entries):**
  - reserved names (`CON`, `NUL`, `AUX`, `COM1`…`LPT9`) → safe suffix (`CON_`);
  - invalid chars (`<>:"|?*`, control) → replace with `_`;
  - trailing dot/space → strip/escape;
  - case-only collision (`README` vs `readme`) on NTFS → auto-rename the second
    (`readme (1)`) + log;
  - duplicate path in one archive → last-wins (default) or rename + log.
  - These apply to **inner** entries during staging; the top-level auto-rename
    (task 04) still governs the placed result.
- **macOS-authored archives:** extract `__MACOSX/`, `._*`, `.DS_Store` **as-is**
  (no silent dropping); `__MACOSX` + one real folder = 2 root entries → wrapped
  (already handled by layout).
- **Decompression bombs:** sanity guard — if declared/estimated uncompressed size
  vastly exceeds free space (or an absurd ratio mid-stream), warn before
  continuing; disk-full path is the backstop; always log when aborted for size.
- **Source-location quirks:** archive on UNC/network share (stage on same share →
  still a rename); read-only media (destination unwritable → "Can't write to this
  folder"); cloud placeholder (OneDrive/Dropbox) hydrates on read → proceed;
  antivirus transiently locks a just-written file → brief retry-with-backoff
  (reuse task-06 helper).
- **Launch & argument edge cases:** no argument → short notice/no-op (not an
  error); multiple paths in one invocation → one-process-per-path per the
  architecture model (document); path no longer exists → "File not found";
  unsupported extension explicitly opened with us → sniff content, else friendly
  "not a supported archive."

## Scope — out
- SFX `.exe` payload sniffing — explicitly a **nice-to-have, not v1-required**
  (design §3); note it as deferred.

## Deliverables
- Symlink/hardlink one-time prompt + copy/skip implementation.
- Inner-name sanitizer completing the task-03 hook (reserved/invalid/trailing/
  case-collision/duplicate).
- Encoding heuristic finalized.
- Decompression-bomb sanity guard + logging.
- Source-location and launch-arg handling.
- Tests/fixtures from the generated corpus (see
  [Testing conventions](README.md#testing-conventions)): link archive copy + skip
  (`edge-links.tar`, incl. the external `/etc/...` target that must be skipped +
  logged), reserved/invalid/trailing-dot names and case-only collision
  (`edge-illegal-names.zip`), duplicate path (`edge-duplicate-path.zip`), macOS
  cruft layout (`edge-macosx.zip`). Environment-dependent cases — UNC source,
  read-only destination, AV-lock retry, decompression-bomb ratio trip, and each
  launch-arg case — are constructed in-test (no static asset). Every test that
  extracts uses a unique temp dir and **deletes it immediately afterward on every
  path**.

## Acceptance criteria
- Link archives show the one-time prompt; Copy and Skip both behave per spec and
  log appropriately; no real symlinks are created.
- Inner names are sanitized without breaking the top-level layout/auto-rename.
- macOS cruft is preserved as-is and wraps correctly.
- The bomb guard warns/aborts and logs; disk-full remains the backstop.
- Source-location quirks behave as documented; arg edge cases don't crash.

## Notes & risks
- The symlink prompt, like the password prompt, must hand off from worker to UI
  without deadlocking the pump (one-time, then cached choice).
- Keep sanitization deterministic so duplicate/case-collision renames are stable.
