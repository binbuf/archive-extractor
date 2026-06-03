# 03 — Extraction engine (libarchive core)

## Goal
Implement the internal extraction interface and its **libarchive** backend:
extract a plan's entries into a temp staging folder on the destination volume,
on a worker thread, reporting progress and honoring cancellation, with safe path
handling. This is the heart of the tool for the common formats.

## Depends on
- [02 — Format detection](02-format-detection.md).

## Design refs
- [01-architecture.md §Extraction Engine, §Threading](../../design/01-architecture.md).
- [03-extraction-behavior.md](../../design/03-extraction-behavior.md) — temp-staging strategy, progress, cancellation, output attributes.
- [06-error-handling.md](../../design/06-error-handling.md) — path safety (zip-slip), long paths.
- [07-passwords-edge-cases.md §7,§8,§10,§11,§12](../../design/07-passwords-edge-cases.md) — special entries, encoding, long paths, large archives, timestamps.

## Scope — in
- An **`IExtractor`** interface (backend-agnostic) consumed by the layout planner
  and UI: `extractToStaging(plan, stagingDir, callbacks)` where callbacks cover
  progress, cancellation token, and (later) password requests.
- **libarchive backend** covering: zip (plain), 7z (plain), tar, gz, bz2, xz,
  zst, lz4, and compound `tar.*` (libarchive auto-applies the filter; read tar
  entries directly). Single-stream inputs decode to exactly one output file.
- **Temp staging:** create a hidden `<workdir>\.archive-extractor-tmp-<rand>\`
  on the **same volume** as the destination; extract everything into it. (The
  `<rand>` must not use `Math.random`-style nondeterminism that breaks
  reproducibility of tests — derive from a process/GUID source.)
- **Worker thread** runs extraction; communicates progress/completion to the UI
  thread via a callback (wired to `PostMessage` in task 05). UI thread never
  blocks.
- **Cancellation:** shared atomic flag checked between entries and during
  chunked writes; abort promptly. On cancel/error, the temp folder is removed
  (deletion helper lives here; layout placement is task 04).
- **Progress:** containers — sum uncompressed entry sizes from headers, advance
  by bytes written; if unknown, advance by compressed bytes consumed vs total.
  Single-stream — drive by compressed bytes read vs file size, else indeterminate.
  Throttle updates (~30–60 ms).
- **Path safety (zip-slip):** normalize each entry path; reject entries that
  escape staging via `..`, are absolute (`C:\`, `\\server\`, leading `/`). On any
  rejected entry, fail the extraction as untrusted (surfaced as error in task 06).
- **Inner-name handling (baseline):** decode names to UTF-16; honor zip UTF-8
  flag (bit 11) with OEM/ANSI fallback heuristic (`hdrcharset`); reserved-name /
  invalid-char sanitization can be stubbed here and completed in task 12, but the
  hook must exist.
- **Output attributes:** apply entry modified time (clamp out-of-range, e.g.
  pre-1980/post-2107); do **not** apply POSIX perms. Preserve empty directories.
- **Special/non-file entries** (device/FIFO/socket): skip + log; never fail the
  whole extraction. Symlinks/hardlinks: detect presence and expose it (full
  copy/skip prompt is task 12) — for now, skip links and log.
- **Long paths & large archives:** all file ops use `\\?\` extended paths;
  64-bit size/counters.

## Scope — out
- Layout decision/placement into the working dir (task 04).
- The 7z.dll backend and brotli backend (tasks 09, 11).
- Password prompting (task 10) — the password callback hook is defined here but
  returns "no password" for now.
- Full inner-name sanitization corner cases (task 12).

## Deliverables
- `IExtractor` + `LibarchiveExtractor`.
- Temp-staging create/cleanup utilities.
- Worker-thread driver with progress + cancellation plumbing (callback-based,
  UI-agnostic so it's testable headless).
- Unit/integration tests over the generated corpus (see
  [Testing conventions](README.md#testing-conventions)) for each libarchive
  format and compound form — `zip__*`, `tar__*`, `7z__*` (plain), the single-
  stream `*.gz/.bz2/.xz/.zst/.lz4`, and every `tar.*` compound — plus a zip-slip
  rejection test on `edge-zip-slip.zip` and a cancellation test. Each test
  extracts into a unique temp dir and **deletes it immediately afterward on every
  path** (including failure), leaving no decompressed output behind.

## Acceptance criteria
- Each libarchive-supported format extracts correctly into staging (byte-for-byte
  for known fixtures).
- A `tar.gz` reads tar entries directly (one flow).
- A malicious `../` or absolute-path entry is rejected and the extraction fails
  as untrusted.
- Cancelling mid-extraction stops promptly and removes the temp folder.
- Timestamps are applied; out-of-range values are clamped, not fatal.
- Staging is always on the destination volume (verified by a same-volume assert).

## Notes & risks
- Decompressing solid archives is handled naturally by extract-everything; only
  progress pacing is affected (see design §5).
- Keep the backend behind `IExtractor` so tasks 09/11 add backends without
  touching layout/UI.
