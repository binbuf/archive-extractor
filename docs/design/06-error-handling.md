# Error Handling & Edge Cases

Guiding rule: **fail loud, fail clean.** Every failure shows a clear error and
leaves the working directory exactly as it was found (temp staging deleted). See
the [temp-staging strategy](03-extraction-behavior.md#temp-staging-strategy-recommended-implementation).

## Failure modes & responses

| Condition | Detection | Response |
|---|---|---|
| Unsupported / unknown format | Detector finds no match | Error dialog; file untouched |
| Corrupt / truncated archive | libarchive read error / CRC failure | "Archive appears to be corrupt"; remove temp |
| Wrong password | Decrypt/CRC failure on encrypted entry | Re-prompt (limited retries); Cancel exits clean. See [Passwords](07-passwords-edge-cases.md) |
| Encryption not supported by engine | Exotic/unsupported cipher (rare — zip via libarchive, 7z/RAR via 7z.dll cover the common cases) | "Encryption not supported" error |
| Multi-volume part missing | Sibling volume absent | Error naming the missing part; see [Passwords & Edge Cases §2](07-passwords-edge-cases.md#2-multi-volume--split-archives) |
| Disk full mid-extract | Write error (`ERROR_DISK_FULL`) | "Not enough disk space"; remove temp |
| No write permission in working dir | Create/move fails (`ERROR_ACCESS_DENIED`) | "Can't write to this folder"; remove temp |
| Path too long (>MAX_PATH) | Create fails | Use `\\?\` extended-length paths to avoid; else error |
| File in use / locked on move | `MoveFileEx` fails | Retry briefly, then error |
| Zip-slip / path traversal entry | Entry path escapes destination after normalization | Skip entry + flag archive as unsafe; surface error |
| Absolute paths in entries | Normalized to relative under destination | Strip drive/leading separators |
| Archive missing / deleted between launch and read | Open fails | "File not found" error |
| Cancel pressed | User action | Abort, remove temp, close (no reveal) |

## Path safety (zip-slip)

Before writing any entry, normalize its path and verify it resolves **inside**
the staging directory. Reject (skip) entries that:

- contain `..` segments that escape the root,
- are absolute (`C:\...`, `\\server\...`, leading `/`),
- contain reserved Windows names (`CON`, `PRN`, `NUL`, `AUX`, `COM1..9`, etc.)
  or invalid characters — sanitize or reject.

If any entry is rejected, treat the archive as untrusted: stop and show an error
rather than silently producing a partial, possibly malicious layout.

## Long paths

Enable long-path support (manifest `longPathAware` + `\\?\`-prefixed paths in the
engine) so deep archive trees extract without hitting the legacy `MAX_PATH`
limit. Document this as a build/manifest requirement.

## Compound-format edge cases

- A single-stream compressor wrapping a tar (`X.tar.gz`) is untarred; a
  single-stream compressor wrapping a non-tar (`page.html.gz`) yields one file.
  The detector decides based on the inner content after the filter, not the
  name alone.
- `.tar.br` (rare): brotli-decode to a temp `.tar`, then untar.

## Empty / degenerate archives

- **Empty archive (0 entries):** nothing to place. Show a brief notice or simply
  complete with no output (implementer's choice — document it). Do not create an
  empty wrapper folder.
- **Single empty folder at root:** unwrapped per the normal 1-entry rule.

## Logging

- For diagnostics, write failures to a rotating log (e.g.
  `%LOCALAPPDATA%\ArchiveExtractor\log.txt`) including format, error code, and
  entry where it failed. No archive contents or passwords are ever logged.
