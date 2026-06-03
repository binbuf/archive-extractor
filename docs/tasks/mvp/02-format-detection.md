# 02 — Format detection & pipeline planning

## Goal
Given an archive path, produce an **extraction pipeline plan**: which backend
handles it, which filter(s) apply, whether it is single-stream or multi-entry,
the archive **stem** for layout/naming, and the engine routing for the
encryption split. This is pure logic — fully unit-testable, no extraction yet.

## Depends on
- [01 — Project scaffolding](01-project-scaffolding.md).

## Design refs
- [02-format-support.md](../../design/02-format-support.md) — format/library matrix, detection strategy, single-stream vs container, compound forms, the brotli special case.
- [01-architecture.md §Format Detector](../../design/01-architecture.md).
- [07-passwords-edge-cases.md §2](../../design/07-passwords-edge-cases.md) — multi-volume naming/first-part detection (stem rules).

## Scope — in
- **Magic-byte sniff** of the file header against known signatures: zip
  (`PK\x03\x04`), 7z (`7z\xBC\xAF\x27\x1C`), rar (`Rar!\x1A\x07`), gzip
  (`\x1F\x8B`), bzip2 (`BZh`), xz (`\xFD7zXZ`), zstd (`\x28\xB5\x2F\xFD`), lz4
  (`\x04\x22\x4D\x18`), tar (`ustar` at offset 257).
- **Extension** as fallback/tiebreaker and as the **sole** signal for `.br`
  (no magic header).
- **Compound-form disambiguation:** `.tar.gz`/`.tgz`, `.tar.bz2`/`.tbz2`,
  `.tar.xz`/`.txz`, `.tar.zst`, `.tar.lz4`, `.tar.br` → produce a
  filter→tar pipeline plan. Bare `.gz`/etc. → single-file plan.
- **Content-vs-extension disagreement:** trust the content; record that it was
  overridden (e.g. a `.zip` that is really 7z).
- **Classification:** single-stream (always 1 entry → place directly) vs.
  container (1..N root entries → layout rules).
- **Engine routing** decision (the plan names the backend):
  - libarchive for non-encrypted formats, plain 7z, tar.*, single-stream, and
    encrypted **zip**;
  - 7z.dll/bit7z for **encrypted 7z** and **all RAR/RAR5**;
  - brotli lib for `.br`.
  - The encryption query itself happens at engine-open time (task 03/09); the
    detector encodes the *routing rule* and a hook to re-route once encryption
    state is known.
- **Archive stem** computation: strip all archive/compression extensions
  (`project.tar.gz` → `project`, `data.7z` → `data`); for single-stream, the
  output name is the inner filename (`page.html.br` → `page.html`, fallback to
  stem). Strip multi-volume suffixes (`name.7z.001` → `name`,
  `name.part1.rar` → `name`, `name.r00`/`name.z01` families).
- **Multi-volume first-part detection:** given any part, identify the family and
  the first part to open (full sibling-completeness check is task 09).
- Unknown/unsupported → a clear "unsupported" plan result (drives the error
  dialog later); file left untouched.

## Scope — out
- Actually opening archives with a backend or querying real encryption state
  (engine tasks). The detector exposes the routing rule + re-route hook only.

## Deliverables
- A `FormatDetector` producing a `PipelinePlan` struct: `{ format, backend,
  filters[], kind (single-stream|container), stem, outputNameHint,
  volumeFamily, firstPartPath, extensionOverriddenByContent }`.
- Signature table + extension table.
- A broad **unit-test suite** over header fixtures and filename cases (incl.
  compound forms, renamed archives, `.br`, multi-volume names, unknown inputs).
  Source real headers from the generated corpus (see
  [Testing conventions](README.md#testing-conventions)): read the leading bytes
  of `tests/assets/` samples for every format, use `edge-renamed-7z-as-zip.zip`
  for the content-vs-extension override, `edge-multivolume.7z.001` for
  multi-volume stem/first-part rules, and `edge-unknown.bin` for the unsupported
  result. Detection reads only header windows — no extraction — so no temp
  cleanup is needed here.

## Acceptance criteria
- Every supported extension and compound form maps to the correct plan.
- `.br` is detected by extension only; a `.tar.br` plans brotli→tar.
- A 7z file renamed to `.zip` is detected as 7z by content and flagged as
  overridden.
- Stems are computed correctly for compound and multi-volume names.
- Unknown content + unknown extension yields the unsupported result.
- Tests pass under `ctest`.

## Notes & risks
- Read only a small header window (e.g. first 512 bytes + offset-257 probe);
  don't read the whole file.
- Keep the detector free of Win32/UI dependencies so it stays in `archive_core`
  and unit-testable.
