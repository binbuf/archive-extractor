# 11 — Brotli (`.br`) + compound-format finalization

## Goal
Add the third backend — Google brotli for `.br` — and finalize compound-format
handling, completing the full format coverage promised by the design.

**Milestone M2:** after this task, every designed format extracts.

## Depends on
- [03 — Extraction engine](03-extraction-engine-libarchive.md) (`IExtractor`, staging).
- [02 — Format detection](02-format-detection.md) (brotli-by-extension, `.tar.br` plan).

## Design refs
- [02-format-support.md §The brotli special case, §Compound formats](../../design/02-format-support.md).
- [06-error-handling.md §Compound-format edge cases](../../design/06-error-handling.md).

## Scope — in
- A **`BrotliExtractor`** implementing `IExtractor` using Google's brotli lib
  (libarchive has no brotli support).
- **Single-file `.br`:** decode the stream to one output file. Output name =
  strip trailing `.br` (`page.html.br` → `page.html`); if nothing remains, fall
  back to the archive stem. Routes through the **single-stream → place directly**
  layout path.
- **`.tar.br` (rare):** brotli-decode to a temp `.tar`, then untar via the
  libarchive backend (apply layout to the **tar's** entries). Optimize the common
  single-file path; treat `.tar.br` as the uncommon branch.
- **Progress:** single-stream — drive by compressed bytes read vs file size, or
  indeterminate (no reliable uncompressed size for brotli).
- **Finalize compound edge cases** across all single-stream filters: a
  compressor wrapping a tar (`X.tar.gz`) is untarred; wrapping a non-tar
  (`page.html.gz`) yields one file — decided by inner content after the filter,
  not the name alone. Confirm this holds uniformly for gz/bz2/xz/zst/lz4/br.

## Scope — out
- Edge-case hardening for names/links/source quirks (task 12).

## Deliverables
- `BrotliExtractor` behind `IExtractor`, statically linked brotli.
- `.tar.br` → temp `.tar` → untar path.
- Tests/fixtures from the generated corpus (see
  [Testing conventions](README.md#testing-conventions)): single-file `.br`
  (`page.html.br` for name-strip; a stemless case for stem fallback), `.tar.br`
  (`tarbr__*`), and a cross-filter compound matrix confirming tar-vs-non-tar inner
  detection for every single-stream filter — `targz/tarbz2/tarxz/tarzst/tarlz4/
  tarbr __*` (tarred) vs the bare single-stream `*.gz/.bz2/.xz/.zst/.lz4/.br`
  (non-tarred). Each test extracts into a unique temp dir and **deletes it
  immediately afterward on every path**.

## Acceptance criteria
- `page.html.br` extracts to `page.html`; a stemless `.br` falls back to the stem.
- `.tar.br` decodes then untars with correct layout.
- For each single-stream filter, a tarred payload is untarred and a non-tarred
  payload yields one file, decided by inner content.
- brotli is only loaded for `.br` inputs (cold start unaffected otherwise).

## Notes & risks
- Brotli has no magic header — detection is extension-only (already in task 02);
  ensure a `.br` mis-detected by content never overrides the extension here.
