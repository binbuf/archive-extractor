# Archive Extractor — Design Documentation

A macOS-Archive-Utility-style extractor for Windows 10/11. Double-clicking a
supported archive opens a small centered dialog, extracts the archive into the
current working directory using sensible "smart" layout rules, and then reveals
the result in File Explorer with the new item pre-selected.

Native C++ for fast startup, a small footprint, and deep OS integration.

## Goals

- **One-click feel.** Double-click → small progress dialog → done → Explorer
  reveals the result. No wizards, no options pages.
- **Smart, non-destructive layout.** Avoid "tarbomb" clutter and avoid
  double-wrapping folders. Never overwrite existing data (auto-rename).
- **Broad format coverage** through a single extraction engine where possible.
- **Native and light.** Raw Win32 UI, native C++, minimal runtime dependencies,
  near-instant cold start.

## Non-goals (initial version)

- Creating/compressing archives (extract-only).
- Browsing archive contents without extracting.
- A persistent main window, settings UI, or background service.
- Batch/queue UI (one process per file for v1; see [Architecture](01-architecture.md)).

## Key product decisions

| Decision | Choice | Rationale |
|---|---|---|
| Packaging | Traditional **WiX/MSI**, per-machine | Maximum flexibility for ProgID + shell registration |
| UI toolkit | **Raw Win32** (common controls) | Lightest, fastest startup, no runtime deps |
| Name collisions | **Auto-rename** (`name (1)`, `name (2)`, …) | Finder-like, never destroys data, zero prompts |
| Extraction engine | **libarchive** primary; **7z.dll** (bit7z) for encrypted 7z + all RAR; Google **brotli** for `.br` | One unified API for most formats; 7z.dll fills libarchive's encrypted-7z/RAR gap |

## Supported formats

`zip, 7z, zst, rar, xz, gz, lz4, tar, bz2, br` — plus compound forms such as
`tar.gz`, `tar.xz`, `tar.bz2`, `tar.zst`, `tar.lz4` (decompress + untar in one
flow). See the [Format Support matrix](02-format-support.md).

## Document index

1. [Architecture](01-architecture.md) — components, process model, data flow.
2. [Format Support](02-format-support.md) — format/library matrix, detection,
   compound formats, the brotli special case.
3. [Extraction Behavior](03-extraction-behavior.md) — smart layout rules,
   temp-staging strategy, auto-rename, passwords, progress, cancellation.
4. [Shell Integration](04-shell-integration.md) — file-type registration
   (WiX/ProgID), launch contract, Explorer open-and-select.
5. [UI Specification](05-ui-spec.md) — the extraction dialog, password prompt,
   error dialog.
6. [Error Handling & Edge Cases](06-error-handling.md) — failure modes and
   recovery.
7. [Passwords & Edge Cases](07-passwords-edge-cases.md) — password flow per
   format, encrypted headers, multi-volume, symlinks, encoding, and the long
   tail of unusual archives.

## Design principles

- **Atomic-ish results.** Extract into a temp staging folder on the *same
  volume* as the destination, then move/rename into place. On error or cancel,
  delete the temp folder so the working directory is never left half-populated.
- **Decide layout from observed contents,** not from guesses — inspect the
  staged root before placing.
- **Fail loud, fail clean.** Clear error dialog; no partial litter left behind.
