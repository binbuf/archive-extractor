# 13 — WiX/MSI per-machine installer

## Goal
Package the app as a per-machine **WiX/MSI** installer that lays down the exe and
`7z.dll`, performs shell registration on install and full cleanup on uninstall,
and leaves Windows in a consistent associations state. This completes the MVP.

**MVP complete** after this task.

## Depends on
- [08 — Shell registration CLI](08-shell-registration-cli.md) (the registration
  logic the installer invokes).
- [09 — 7z.dll backend](09-7zdll-backend.md) (the `7z.dll` payload to ship).
- All extraction backends (03, 09, 11) so the shipped exe is feature-complete.

## Design refs
- [04-shell-integration.md §File-type registration (WiX/per-machine), §CLI hooks](../../design/04-shell-integration.md).
- [README.md — Packaging decision (WiX/MSI, per-machine)](../../design/README.md).
- [01-architecture.md §External dependencies](../../design/01-architecture.md).

## Scope — in
- A **WiX** project producing a **per-machine x64 MSI**.
- **Payload:** `ArchiveExtractor.exe` + `7z.dll` (and any non-statically-linked
  runtime), installed under Program Files.
- **Registration on install / removal on uninstall:** write the ProgID,
  per-extension `OpenWithProgids`, and capabilities (single source of truth in
  the task-08 CLI) — either by calling `--register` / `--unregister` as custom
  actions **and/or** declaring the registry directly in WiX. Reuse the shared
  extension-list constant so the installer and CLI never drift.
- **`SHChangeNotify(SHCNE_ASSOCCHANGED, ...)`** after install/uninstall so the
  shell refreshes associations.
- **Upgrade logic:** `MajorUpgrade` so reinstalling/upgrading replaces cleanly
  without orphaned keys or duplicate associations.
- **Uninstall is clean:** all registry keys and files removed; no orphaned
  associations or ProgID left behind.
- Honor the **default-handler limitation:** the installer never fabricates the
  hashed `UserChoice`; for owned extensions (e.g. `.zip`) it only advertises via
  *Open with* / *Default apps* (the user opts in via the OS picker — task 08).

## Scope — out
- Code-signing/notarization and auto-update (post-MVP; note as follow-ups).

## Deliverables
- A WiX source set (`installer/`) building `ArchiveExtractor.msi`.
- Custom actions (or direct registry components) for register/unregister tied to
  install/uninstall.
- `MajorUpgrade` configuration.
- A documented build step producing the MSI as a release artifact.
- A clean-machine/VM verification: install → double-click each format works →
  app shows in *Default apps* → uninstall → associations and files fully removed.

## Acceptance criteria
- The MSI installs per-machine, lays down the exe + `7z.dll`, and registers
  associations such that double-click extraction works for all formats.
- Uninstall removes all files and registry keys with no orphaned associations.
- Reinstall/upgrade replaces cleanly (no duplicate ProgID or stale keys).
- The shell reflects associations immediately (via `SHChangeNotify`).
- No fabricated `UserChoice`; owned-ext default-setting goes through the OS picker.

## Notes & risks
- Keep registration logic in the CLI (task 08) as the single source of truth;
  prefer custom actions over duplicating registry tables where practical, but
  ensure rollback on failed install.
- `7z.dll` must match the bit7z version the exe expects — pin and verify.
- Code-signing is strongly recommended before real distribution (SmartScreen);
  flag as the immediate post-MVP follow-up.
