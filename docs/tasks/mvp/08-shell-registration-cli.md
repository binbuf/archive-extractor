# 08 — Shell registration CLI (`--register` / `--unregister`)

## Goal
Implement the real `--register` / `--unregister` logic: write the ProgID,
per-extension associations, and Registered-Application capabilities so
double-click launches the app and it appears in *Open with* / *Default apps*.
This is the single source of truth the installer (task 13) will invoke.

**Milestone M1:** after this task, double-clicking a libarchive-supported
archive performs the full extract → reveal flow.

## Depends on
- [07 — Shell reveal](07-shell-reveal.md) (so a real double-click does something useful).

## Design refs
- [04-shell-integration.md §File-type registration, §CLI hooks](../../design/04-shell-integration.md).

## Scope — in
- **ProgID** under `HKLM\Software\Classes\ArchiveExtractor.Archive.1`:
  - default = `"Compressed Archive"`,
  - `DefaultIcon` = `"<install>\ArchiveExtractor.exe,0"`,
  - `shell\open\command` = `"\"<install>\ArchiveExtractor.exe\" \"%1\""`.
- **Per-extension association** for `.zip .7z .zst .rar .xz .gz .lz4 .tar .bz2
  .br` and the `.tgz`/`.tbz2`/`.txz` family via `OpenWithProgids`.
- **Capabilities / Registered Application:**
  - `HKLM\Software\ArchiveExtractor\Capabilities` with `ApplicationName`,
    `ApplicationDescription`, and `FileAssociations\.<ext>` entries;
  - `HKLM\Software\RegisteredApplications\Archive Extractor` →
    `"Software\ArchiveExtractor\Capabilities"`.
- **Default-handler limitation handling:**
  - For an extension **no app owns**, registering the ProgID as class default can
    make us the handler directly.
  - For **owned** extensions (notably `.zip`), do **not** fabricate the hashed
    `UserChoice`. Provide an optional one-confirmation path
    (`SHOpenWithDialog` / Default-Apps deep-link) to let the **user** set us as
    default. Wire as a first-run/menu action (document the trigger).
- **Idempotency:** `--register` is safe to re-run; `--unregister` removes
  everything it wrote.
- Broadcast `SHChangeNotify(SHCNE_ASSOCCHANGED, ...)` after changes so the shell
  refreshes.
- Requires elevation (HKLM); document that these are run elevated (by the
  installer, or manually from an elevated prompt).

## Scope — out
- The WiX/MSI packaging itself (task 13) — but the installer will call this CLI,
  so keep the logic self-contained and idempotent.

## Deliverables
- `--register` / `--unregister` implementations replacing the task-01 stubs.
- Registry writer covering ProgID, per-ext `OpenWithProgids`, and capabilities.
- The optional user-driven default-handler picker for owned extensions.
- Test/verification script: register on a test machine/VM, confirm double-click
  launches the app and the app shows in *Open with* / *Default apps*, then
  unregister and confirm full removal.

## Acceptance criteria
- After `--register` (elevated), double-clicking a supported (unowned-ext)
  archive launches the app and runs the extract→reveal flow.
- The app appears in *Open with* and *Default apps* for all registered
  extensions, including `.zip`.
- We never write a fabricated `UserChoice` hash; setting default for an owned ext
  goes through the OS picker.
- `--unregister` removes all keys; `--register` is idempotent on re-run.

## Notes & risks
- HKLM writes need elevation — fail with a clear message if not elevated.
- Keep the exact extension list in one shared constant reused by the installer.
