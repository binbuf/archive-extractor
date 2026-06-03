# Passwords & Edge Cases

This is the long tail: how passwords actually work per format, and the unusual
archives that break naive extractors. Companion to
[Extraction Behavior](03-extraction-behavior.md) and
[Error Handling](06-error-handling.md).

---

## 1. Password-protected archives

### Engine routing (recap)

libarchive can decrypt **zip** but **not** 7z or RAR (it returns −30,
*"Reading encrypted data is not currently supported"*,
[libarchive#2516](https://github.com/libarchive/libarchive/issues/2516)). So:

| Encrypted input | Backend | Notes |
|---|---|---|
| zip (ZipCrypto / WinZip AES) | libarchive | `archive_read_add_passphrase()` |
| 7z (AES-256) | **7z.dll** (bit7z) | `setPassword()` before extract |
| RAR / RAR5 (AES) | **7z.dll** (bit7z) | all RAR routed here regardless |

### When is the password requested?

Two timings, depending on what is encrypted:

- **Encrypted data, clear headers (typical):** the file list is readable, so we
  enumerate and plan layout first, then prompt for the password when extraction
  needs it. Detect via `archive_read_has_encrypted_entries()` (libarchive) or
  the per-item `IsEncrypted` property (7z.dll).
- **Encrypted headers** — 7z made with `-mhe=on`, or RAR made with `-hp`. The
  **file list itself is encrypted**, so we cannot even enumerate entries (or
  know the layout/names) without the password. Here we must **prompt first**,
  before any progress or layout analysis. The detector recognizes this state
  (opening the archive reports encryption / yields no listing) and shows the
  [password prompt](05-ui-spec.md) up front.

In both cases the prompt is shown **before** the determinate progress bar can be
meaningful for the encrypted portion; for encrypted-header archives the dialog
sequence is: password prompt → (on success) progress dialog.

### Mixed and per-entry passwords

- **Mixed archives:** zip permits some entries encrypted and others not. We
  prompt only when the first encrypted entry is reached (clear-header case),
  then proceed.
- **Per-entry differing passwords** (rare; zip allows it): the cached passphrase
  is tried first; on failure for a specific entry we re-prompt for that entry.
  libarchive can hold multiple registered passphrases and tries each.

### Retry, caching, and cancel

- **Cache** the accepted passphrase for the lifetime of this one extraction and
  reuse it for every subsequent entry — never re-prompt per file for a
  single-password archive.
- **Wrong password:** decryption/CRC failure → re-show the prompt with an inline
  "Incorrect password" message. Cap retries (e.g. 3); after the cap or on
  Cancel, abort the whole extraction and delete the temp staging (nothing left
  behind).
- A password supplied for one archive is **never** reused for another process /
  archive.

### Security requirements

- Mask the input field; do not echo.
- **Never** write the password to logs, temp files, the registry, or telemetry.
- Hold it only in memory for the extraction's duration; **zero the buffer**
  after use. Avoid swapping it to disk where practical.
- Do not offer "remember this password" in v1.

---

## 2. Multi-volume / split archives

A single logical archive split across files. Double-clicking one part must
extract the whole set.

| Family | Naming | Handling |
|---|---|---|
| 7z split | `name.7z.001`, `.002`, … | Point the engine at `.001`; it reads the rest. |
| RAR (new) | `name.part1.rar`, `name.part2.rar`, … | 7z.dll follows the chain from `part1`. |
| RAR (old) | `name.rar`, `name.r00`, `name.r01`, … | 7z.dll follows from `.rar`. |
| zip spanned | `name.zip` + `name.z01`, `name.z02`, … | Needs all parts present. |

Behavior:
- If launched on a **non-first** part (e.g. `.002`, `.r01`, `.partN.rar`),
  locate the **first** part in the same folder and start there.
- **Completeness check:** verify the expected sibling parts exist. If a part is
  missing, show a clear error naming the missing volume rather than producing a
  truncated/corrupt result.
- Layout/auto-rename uses the base name with all volume + archive suffixes
  stripped (`name.7z.001` → stem `name`).

---

## 3. Self-extracting (SFX) and leading-junk archives

- **SFX `.exe`:** a real executable with an archive appended. Double-clicking an
  `.exe` runs it (not our concern, and we do **not** register `.exe`). If a user
  explicitly "opens with" Archive Extractor, we may sniff for a trailing
  zip/7z/rar payload and extract it; otherwise show "not a supported archive."
  Treat as a nice-to-have, not v1-required.
- **Leading junk before a zip:** zip's central directory lives at the end of the
  file, so a zip with a prepended stub still extracts correctly — libarchive
  finds the directory from the tail. We rely on this rather than special-casing.

---

## 4. Nested archives

We extract **one level only**. `outer.zip` containing `inner.7z` yields
`inner.7z` on disk — we do **not** auto-recurse. (The single exception is
compound compression like `tar.gz`, which is one logical archive and is fully
expanded — see [Format Support](02-format-support.md).)

---

## 5. Solid archives (7z / RAR)

In a solid archive, entries share a compression stream, so extracting any file
requires decompressing the ones before it. Our model extracts everything, so
this is correct by construction — it only affects progress pacing (early
entries may appear to "wait" on the stream). No special handling beyond honoring
cancellation between flushes.

---

## 6. Symlinks & hardlinks

tar (and some other) archives can carry symbolic and hard links. On Windows,
creating a symlink normally requires elevated privilege or Developer Mode.

**Behavior (per the product decision):** when the archive contains links, show a
**one-time prompt** for the whole extraction:

> *"This archive contains links. Copy the files they point to, or skip the
> links?"* — **[Copy targets]** / **[Skip links]**

- **Copy targets:** replace each link with a copy of its target's contents. No
  privilege needed. Note: self-referential or external (`/etc/...`,
  out-of-archive) targets can't be copied → those individual links are skipped
  and logged.
- **Skip links:** omit link entries entirely; log what was skipped.
- We do **not** attempt to create real Windows symlinks/junctions in v1 (privilege
  + security surface). The prompt appears at most once per extraction; the choice
  applies to all links.
- **Hardlinks** within the archive are materialized as independent copies (Copy)
  or skipped (Skip), matching the chosen mode.

---

## 7. Special / non-file entries

Device nodes, FIFOs, sockets, and similar POSIX special entries in tar are not
meaningful on Windows. **Skip** them and log; never fail the whole extraction
over them. Empty **directories** are preserved (created even with no children).

---

## 8. Filename encoding

- **zip:** entry names are either UTF-8 (general-purpose bit 11 / EFS set) or a
  legacy code page (often CP437 or the creator's OEM page). Use UTF-8 when the
  flag is set; otherwise decode via a best-effort heuristic (try UTF-8, fall
  back to the system OEM/ANSI code page). libarchive's `hdrcharset` option can
  steer this. Mis-decoding shows as mojibake — getting this right matters for
  non-ASCII names.
- **7z / RAR:** store names as UTF-16/UTF-8 already; decode directly.
- Normalize all names to UTF-16 for Windows filesystem APIs.

---

## 9. Illegal names, collisions, duplicates

Windows filesystem constraints the archive may violate:

| Problem | Handling |
|---|---|
| Reserved names (`CON`, `NUL`, `AUX`, `COM1`…`LPT9`) | Append a safe suffix (e.g. `CON_`) |
| Invalid chars (`<>:"\|?*`, control chars) | Replace with `_` |
| Trailing dot or space (`name.` / `name `) | Strip or escape (illegal on NTFS) |
| **Case-only collision** (`README` vs `readme`) | NTFS is case-insensitive → the second collides; auto-rename it (`readme (1)`) and log |
| **Duplicate path** in one archive (same entry twice) | Last-wins by default, or rename; log the duplicate |

These sanitizations apply to **inner** entries during staging; the top-level
[auto-rename](03-extraction-behavior.md#name-collision-handling--auto-rename)
rule still governs the placed result.

---

## 10. Long paths

Deep archive trees easily exceed the legacy `MAX_PATH` (260). Enable long-path
support (`longPathAware` manifest entry + `\\?\`-prefixed paths in all engine
file operations) so extraction doesn't fail partway. Build/manifest requirement.

---

## 11. Large archives

- **>4 GB files or >65,535 entries:** requires Zip64 — ensured by a 64-bit build
  and libarchive's Zip64 support. No special handling beyond building 64-bit.
- Progress counters and size accumulators must be 64-bit.

---

## 12. Timestamps

- zip stores DOS times (range 1980–2107, 2-second resolution); other formats
  carry Unix times. Apply the entry's modified time where available.
- Clamp out-of-range values (pre-1980, post-2107, or beyond what the FS accepts)
  to a valid time rather than failing the write.

---

## 13. macOS-authored archives

zips made on macOS often contain a `__MACOSX/` folder, `._<name>` AppleDouble
sidecars, and `.DS_Store` files. v1 extracts these **as-is** (no silent
dropping) so behavior is predictable; an option to filter macOS cruft is a
possible later enhancement. Note that a `__MACOSX` + one real folder counts as
**2 root entries**, so layout wraps them — acceptable, and avoids guessing.

---

## 14. Source-location quirks

| Situation | Handling |
|---|---|
| Archive on a **UNC / network share** | Stage on the same share so the final move is still a rename; works as normal |
| Archive on **read-only media** (CD/DVD, read-only mount) | Destination = the archive's folder, which is unwritable → "Can't write to this folder" error. (Optional later: offer Downloads as a fallback target) |
| **Cloud placeholder** source (OneDrive/Dropbox on-demand) | Reading hydrates the file automatically; proceed normally |
| **Antivirus** locks a just-written file | Move fails transiently → brief retry-with-backoff before erroring |

---

## 15. Decompression bombs

Highly compressed archives can expand to enormous sizes. The
[disk-full path](06-error-handling.md) is the backstop. Optionally add a sanity
guard: if the declared/estimated uncompressed size vastly exceeds free space (or
an absurd ratio is detected mid-stream), warn the user before continuing. Always
[`log()`](06-error-handling.md#logging) when extraction is aborted for size.

---

## 16. Launch & argument edge cases

- **No argument:** show a short "open an archive to extract it" notice (or do
  nothing); not an error.
- **Multiple paths passed in one invocation** (e.g. some shells batch a
  multi-select): v1 processes the **first** and ignores the rest, or spawns one
  process per path — pick one and document it; the per-file process model
  ([Architecture](01-architecture.md)) favors one-process-per-file.
- **Path no longer exists** (deleted between launch and open): "File not found."
- **Unsupported extension explicitly opened with us:** sniff content; if still
  unrecognized, friendly "not a supported archive" error.

---

## 17. Degenerate archives

Covered in [Error Handling](06-error-handling.md): empty archives (no output, no
empty wrapper), single empty folder at root (unwrapped), corrupt/truncated
archives (clear error, temp removed).
