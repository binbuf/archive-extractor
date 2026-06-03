# Format Support

## Format / library matrix

| Ext | Kind | Engine | Encryption | Notes |
|---|---|---|---|---|
| `.zip` | Container | libarchive | **Yes** — ZipCrypto + WinZip AES, via libarchive | Most common; Explorer already owns this ext |
| `.7z` | Container | libarchive (plain) / **7z.dll** (encrypted) | **Yes** — AES-256, via 7z.dll backend | libarchive cannot decrypt 7z (see below); solid archives supported |
| `.rar` | Container | **7z.dll** (preferred) / libarchive (fallback) | **Yes** — RAR/RAR5 + AES, via 7z.dll | Extract-only; routed to 7z.dll for fidelity + crypto |
| `.tar` | Container | libarchive | n/a | Pure archive, no compression |
| `.gz` | Single-stream | libarchive (gzip filter) | No | Decompresses to one file |
| `.bz2` | Single-stream | libarchive (bzip2 filter) | No | One file |
| `.xz` | Single-stream | libarchive (xz/lzma filter) | No | One file |
| `.zst` | Single-stream | libarchive (zstd filter) | No | One file |
| `.lz4` | Single-stream | libarchive (lz4 filter) | No | One file |
| `.br` | Single-stream | **Google brotli lib** | No | **Not in libarchive** — see below |

### Engine routing & the encryption split

libarchive is the primary engine, but it **cannot decrypt 7z or RAR data** — it
returns error −30, *"Reading encrypted data is not currently supported"*
([libarchive#2516](https://github.com/libarchive/libarchive/issues/2516)). It
can decrypt **zip** fully. So:

| Input | Engine |
|---|---|
| Any non-encrypted format (incl. plain 7z, tar.*, single-stream) | **libarchive** |
| Encrypted **zip** | **libarchive** (passphrase) |
| Encrypted **7z** | **7z.dll** (bit7z) |
| Any **RAR / RAR5** (encrypted or not) | **7z.dll** (bit7z) — better fidelity than libarchive's clean-room RAR reader |

The detector decides routing up front: it sniffs the format, then queries
encryption state (`archive_read_has_encrypted_entries()` for libarchive-handled
types). If the input is encrypted-and-7z, or any RAR, it hands off to the 7z.dll
backend. See [Passwords & Edge Cases](07-passwords-edge-cases.md) for the full
password flow and the encrypted-header case (where even the file list is
encrypted and the password is required before enumeration).

### Compound (filter + tar) formats

These decompress and untar in a single flow. libarchive auto-applies the filter,
so we read tar entries directly:

| Ext(s) | Pipeline |
|---|---|
| `.tar.gz`, `.tgz` | gzip → tar |
| `.tar.bz2`, `.tbz2` | bzip2 → tar |
| `.tar.xz`, `.txz` | xz → tar |
| `.tar.zst` | zstd → tar |
| `.tar.lz4` | lz4 → tar |

Any single-stream compressor wrapping a tar (`X.tar` inside `X.tar.Y`) is treated
as a true multi-entry archive: we apply the [layout rules](03-extraction-behavior.md)
to the **tar's** entries, not to the intermediate `.tar` file.

## The brotli special case

libarchive deliberately does **not** support brotli: the raw `.br` stream has no
magic-number header, so it cannot be auto-detected by content
([libarchive#1238](https://github.com/libarchive/libarchive/issues/1238)). We
handle `.br` ourselves:

- Detect `.br` **by file extension only**.
- Decode the stream with Google's [brotli](https://github.com/google/brotli)
  library to a single output file.
- Output filename: strip the trailing `.br` (`page.html.br` → `page.html`); if
  nothing remains, fall back to the archive stem.
- A `.tar.br` (uncommon) → brotli-decode to a temp `.tar`, then untar. Most `.br`
  in the wild are single-file web assets, so optimize for the single-file path.

## Detection strategy

1. **Magic-byte sniff** of the first bytes against known signatures
   (`PK\x03\x04` zip, `7z\xBC\xAF\x27\x1C`, `Rar!\x1A\x07`, `\x1F\x8B` gzip,
   `BZh` bzip2, `\xFD7zXZ` xz, `\x28\xB5\x2F\xFD` zstd, `\x04\x22\x4D\x18` lz4,
   `ustar` tar at offset 257, …).
2. **Extension** disambiguates compound forms (`.tar.gz` vs bare `.gz`) and is
   the **sole** signal for `.br`.
3. If the content sniff and extension disagree (e.g. a `.zip` that is really a
   7z), trust the **content** and log it; this is how a renamed archive still
   extracts correctly.
4. Unknown/unsupported → friendly error dialog; the file is left untouched.

## Single-stream vs multi-entry — why it matters

| Kind | Entry count | Layout outcome |
|---|---|---|
| Single-stream (`.gz`, `.bz2`, `.xz`, `.zst`, `.lz4`, `.br`) | Always exactly 1 | Extract the one file directly into the working dir |
| Container (`.zip`, `.7z`, `.rar`, `.tar`, `tar.*`) | 1..N root entries | Apply the smart-layout rules |

This distinction lets the engine skip root-analysis for single-stream inputs.
