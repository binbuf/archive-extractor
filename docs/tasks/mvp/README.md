# MVP Task Plan

Sequential implementation plan to reach the v1 MVP described in
[`docs/design`](../../design/README.md). Scope confirmed as **the full design**:
all listed formats including RAR + encrypted-7z (via the 7z.dll/bit7z backend),
brotli (`.br`), password-protected archives, and a per-machine **WiX/MSI**
installer.

## How to use this plan

Tasks are numbered in build order. Each builds on the previous, and each is
written to leave the project in a runnable, testable state. Work them in
sequence; the `Depends on` field records the hard prerequisites.

Each task file follows the same shape: **Goal · Depends on · Design refs ·
Scope (in/out) · Deliverables · Acceptance criteria · Notes & risks.**

## Task list

| # | Task | Milestone |
|---|---|---|
| [01](01-project-scaffolding.md) | Project scaffolding & build system | |
| [02](02-format-detection.md) | Format detection & pipeline planning | |
| [03](03-extraction-engine-libarchive.md) | Extraction engine — libarchive core | |
| [04](04-layout-planner.md) | Layout planner & atomic placement | |
| [05](05-ui-extraction-dialog.md) | UI — extraction progress dialog | |
| [06](06-error-handling.md) | Error handling, error dialog & logging | |
| [07](07-shell-reveal.md) | Shell reveal (open-and-select in Explorer) | |
| [08](08-shell-registration-cli.md) | Shell registration CLI (`--register`) | **M1: double-click works for libarchive formats** |
| [09](09-7zdll-backend.md) | 7z.dll / bit7z backend (RAR + encrypted-7z, multi-volume) | |
| [10](10-passwords.md) | Password-protected archives (prompt + all backends) | |
| [11](11-brotli-and-compound.md) | Brotli (`.br`) + compound-format finalization | **M2: full format coverage** |
| [12](12-edge-case-hardening.md) | Edge-case hardening (links, names, source quirks, bombs) | |
| [13](13-wix-installer.md) | WiX/MSI per-machine installer | **MVP complete** |

## Milestones

- **M1 (after task 08).** Double-clicking a `zip / 7z(plain) / tar / gz / bz2 /
  xz / zst / lz4` archive (and the compound `tar.*` forms) extracts it with the
  smart layout, shows the progress dialog, handles errors cleanly, and reveals
  the result in Explorer. Registration is done via the `--register` CLI.
- **M2 (after task 11).** All designed formats extract, including RAR,
  encrypted-7z, encrypted-zip with the password flow, and `.br`.
- **MVP (after task 13).** Shipped as an installable per-machine MSI that
  registers/unregisters cleanly.

## Conventions for implementers

- 64-bit build only (Zip64, >4 GB files, 64-bit counters).
- All engine file I/O uses `\\?\`-extended paths; manifest is `longPathAware`.
- Per-monitor DPI awareness v2 and a comctl32 v6 manifest for native theming.
- Statically link libarchive and its backends where possible; `7z.dll` is the
  one shipped runtime DLL, loaded on demand.
- Never overwrite user data; never leave partial litter (temp-staging is
  all-or-nothing).

## Testing conventions

All archive-based tests draw from one **generated corpus**, never from
hand-checked-in binaries:

- **Generator:** [`scripts/gen-test-assets.py`](../../../scripts/gen-test-assets.py)
  produces one sample of every kind of input the design covers — every extension
  and compound form, all five root structures (single-file, single-folder,
  multi-files, multi-folders, mixed), every password variant, and the edge cases
  (empty, single-empty-folder, macOS cruft, nested, zip-slip, illegal/reserved
  names, duplicate path, content-vs-extension mismatch, truncated, unknown input,
  symlink/hardlink tar, and a 7z multi-volume split).
- **Location & manifest:** assets are written to `tests/assets/` (which is
  **git-ignored** — regenerate, don't commit) alongside a `manifest.json` listing
  each asset's `format`, `structure`, `password`, and **expected extraction
  outcome**. Tests should key off `manifest.json` rather than hard-coding lists.
- **Regenerate** before running the suite: `python scripts/gen-test-assets.py`
  (add `--clean` to rebuild from scratch). CI runs this as a pre-test step.
- **Password** for every encrypted asset is `test1234`.
- **RAR** assets (`rar__*`, `rar-aes*`) require WinRAR's `rar.exe` on `PATH`
  (RAR is proprietary and cannot be created otherwise); the generator emits them
  when `rar.exe` is found and **skips** them otherwise. RAR tests must be gated on
  asset presence so the suite is green on machines without WinRAR.

**Clean up decompressed output immediately.** Any test that extracts or
decompresses must:

- extract into a **unique temp directory** (e.g. under the test's `TEMP`), never
  into the source tree or `tests/assets/`;
- **delete that directory immediately after** the assertions, on **every** path
  including failures and exceptions (RAII / gtest `TearDown` / `try…finally`), so
  no decompressed files ever persist between tests;
- leave the input corpus untouched.

This mirrors the tool's own all-or-nothing temp-staging guarantee and keeps the
working tree free of test litter.
