# Extraction Behavior

## Smart layout rules

The goal mirrors macOS Archive Utility: avoid both "tarbombs" (loose files
dumped into the working directory) and pointless double-wrapping
(`project/project/...`).

Decision is based on the number of **distinct root-level entries** in the
archive:

| Archive root contains | Result in working directory |
|---|---|
| Exactly **1 entry** (a single file) | Extract that file directly |
| Exactly **1 entry** (a single folder) | Extract that folder directly (no extra wrapper) |
| **2+ entries** (any mix of files/folders) | Create a folder named after the archive and place everything inside |

This collapses to one rule:

> **Count distinct root entries. 1 → place directly. ≥2 → wrap in a folder named
> after the archive stem.**

Worked examples (archive `project.zip`):

| Archive root layout | Output |
|---|---|
| `readme.txt` | `./readme.txt` |
| `src/`, `readme.txt`, `LICENSE` | `./project/{src/, readme.txt, LICENSE}` |
| `project/` (with files inside) | `./project/...` (unwrapped, not `project/project/`) |
| `a/`, `b/` | `./project/{a/, b/}` |

**Archive stem** = filename with all archive/compression extensions removed:
`project.tar.gz` → `project`, `data.7z` → `data`. Single-stream outputs use the
inner filename instead (`page.html.br` → `page.html`); see
[Format Support](02-format-support.md).

## Temp-staging strategy (recommended implementation)

Rather than pre-scanning the archive to decide layout and then extracting (which
can mean decompressing solid archives twice), extract **once into a temp staging
folder**, then inspect and place:

1. Create a hidden temp folder on the **same volume** as the destination, e.g.
   `<workdir>\.archive-extractor-tmp-<rand>\`. Same-volume ensures the final
   move is a rename, not a copy.
2. Extract all entries into the temp folder, reporting progress.
3. Inspect the temp folder's root entries → apply the layout rule above.
4. **Place** results into the working directory:
   - 1 root entry → move that entry out, applying auto-rename if needed.
   - ≥2 root entries → rename the temp folder itself to the archive stem (or
     create the stem folder and move contents), applying auto-rename if needed.
5. Delete the temp folder (it is empty or removed by the rename).

On **cancel or error at any step**, delete the temp folder. Because nothing was
ever written directly into the working directory, the result is all-or-nothing
and the working directory is never left with partial litter.

> Note: keep staging on the destination volume specifically so step 4 is an
> `O(1)` `MoveFileEx` rename. Falling back to the system temp dir on another
> volume would force a full copy.

## Name-collision handling — auto-rename

Never overwrite. When the chosen output name already exists in the working
directory, append a counter, Windows-style:

```
project        → project (1) → project (2) → ...
report.pdf     → report (1).pdf → report (2).pdf   (counter before extension)
archive.tar.gz → handled by stem: project, project (1), ...
```

- Rename only the **top-level placed item** (the single root entry, or the
  wrapper folder). Inner contents are never individually renamed.
- For files, insert the counter **before the final extension**; for folders,
  append at the end.
- Probe incrementally (`(1)`, `(2)`, …) until a free name is found.

## Password-protected archives

Full behavior — per-format support, encrypted headers, mixed/per-entry
passwords, retries, caching, and security — lives in
**[Passwords & Edge Cases](07-passwords-edge-cases.md)**. In brief:

- Encrypted **zip** is handled by libarchive; encrypted **7z** and all **RAR**
  go through the 7z.dll backend (libarchive can't decrypt 7z/RAR).
- Flow: detect encryption → show [password prompt](05-ui-spec.md) → supply the
  passphrase → extract. Cache it for the rest of the archive's entries.
- **Wrong password:** re-prompt with a limited retry count; Cancel exits cleanly
  (temp removed). The password is never logged or persisted.

## Progress reporting

- **Containers:** sum uncompressed entry sizes (available from headers during
  extraction) for a determinate bar; advance by bytes written. If sizes are
  unknown (some streamed entries), advance by compressed bytes consumed vs total
  compressed size.
- **Single-stream (`.gz`/`.br`/etc.):** uncompressed size is generally unknown,
  so drive the bar by **compressed bytes read vs file size** (a good proxy), or
  fall back to a marquee/indeterminate bar.
- Update at a throttled rate (e.g. ~30–60 ms) to avoid UI churn.

## Cancellation

- Cancel sets a shared atomic flag checked between entries and during chunked
  writes; the worker aborts promptly.
- The temp folder is deleted; the dialog closes. No Explorer reveal on cancel.

## Output attributes

- Preserve file modification times from the archive where available.
- Do **not** preserve POSIX permission/ownership semantics (not meaningful on
  Windows); apply normal inherited ACLs.
- Sanitize entry paths: reject absolute paths and `..` traversal that would
  escape the destination (zip-slip protection). See
  [Error Handling](06-error-handling.md).
- **Symlinks/hardlinks (tar, etc.):** if the archive contains links, show a
  one-time prompt asking whether to **copy link targets** or **skip links**;
  apply the choice to the whole extraction. See
  [Passwords & Edge Cases](07-passwords-edge-cases.md#symlinks--hardlinks).
