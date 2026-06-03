# 09 — 7z.dll / bit7z backend (RAR + encrypted-7z, multi-volume)

## Goal
Add the second extraction backend behind `IExtractor`: bit7z over `7z.dll`,
covering all RAR/RAR5 and encrypted-7z (which libarchive cannot decrypt), plus
multi-volume chains. Routing decided by the detector (task 02) and the
encryption query.

## Depends on
- [03 — Extraction engine](03-extraction-engine-libarchive.md) (the `IExtractor`
  interface and staging/progress/cancel plumbing).
- [02 — Format detection](02-format-detection.md) (routing + multi-volume detection).

## Design refs
- [02-format-support.md §Engine routing & the encryption split](../../design/02-format-support.md).
- [01-architecture.md §External dependencies](../../design/01-architecture.md).
- [07-passwords-edge-cases.md §2, §5](../../design/07-passwords-edge-cases.md) — multi-volume, solid archives.

## Scope — in
- **bit7z / 7z.dll integration:** ship `7z.dll` alongside the exe; **load on
  demand** so it never affects cold start for libarchive cases.
- A **`SevenZipExtractor`** implementing `IExtractor`, mapping bit7z extraction,
  progress, and cancellation onto the same staging model and callbacks.
- **Routing:** the engine selects this backend for:
  - **encrypted 7z** (after the encryption query),
  - **all RAR / RAR5** regardless of encryption (better fidelity than
    libarchive's clean-room RAR reader).
- **Encryption-state query** that completes the detector's re-route hook: open
  via libarchive, check `archive_read_has_encrypted_entries()` for 7z; per-item
  `IsEncrypted` for 7z.dll; route accordingly.
- **Multi-volume / split archives:**
  - 7z split `name.7z.001…` → point bit7z at `.001`;
  - RAR new `name.partN.rar` → follow from `part1`;
  - RAR old `name.rar` + `.r00…` → follow from `.rar`;
  - if launched on a **non-first** part, locate and open the **first** part in the
    same folder;
  - **completeness check:** verify expected sibling parts exist; if one is
    missing, error naming the missing volume (no truncated result).
  - layout/auto-rename uses the volume-stripped stem (task 02).
- **Solid archives:** handled by extract-everything; only progress pacing
  differs — honor cancellation between flushes.

## Scope — out
- The password **prompt UI** and the full password flow (task 10) — this task
  exposes the password callback to bit7z (`setPassword`) but the interactive
  prompt is added next.
- brotli (task 11).

## Deliverables
- `SevenZipExtractor` (bit7z) behind `IExtractor`, with on-demand `7z.dll` load.
- Encryption-query + routing completion.
- Multi-volume first-part resolution + completeness check.
- The `7z.dll` is included in the build output next to the exe (and later in the
  MSI payload — task 13).
- Tests/fixtures from the generated corpus (see
  [Testing conventions](README.md#testing-conventions)): plain 7z (`7z__*`),
  encrypted 7z (`7z-aes*.7z`), 7z multi-volume split
  (`edge-multivolume.7z.001…005`) — including launching on a non-first part and a
  missing-volume error case (delete one part in the temp copy) — and the RAR
  family (`rar__*`, plain + RAR5 + multi-part). RAR assets exist only when the
  generator found WinRAR's `rar.exe`; **gate the RAR tests on asset presence** so
  the suite stays green without WinRAR. Each test extracts into a unique temp dir
  and **deletes it immediately afterward on every path**.

## Acceptance criteria
- RAR / RAR5 archives extract via 7z.dll with correct layout and reveal.
- Encrypted-7z is routed to 7z.dll (extraction proceeds once a password is
  supplied — full flow lands in task 10).
- Multi-volume sets extract from any part; a missing part yields the named-volume
  error and clean cleanup.
- `7z.dll` is loaded only when this backend is used (verify it isn't loaded for a
  plain zip).

## Notes & risks
- bit7z is a static wrapper but needs the runtime `7z.dll`; keep versions matched
  and documented.
- Keep progress/cancel semantics identical to the libarchive backend so the UI is
  backend-agnostic.
