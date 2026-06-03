# Architecture

## Process model

Archive Extractor is a **short-lived, single-shot process**. The shell launches
one instance per archive, passing the archive path on the command line:

```
ArchiveExtractor.exe "C:\Users\dan\Downloads\project.tar.gz"
```

- One archive → one process → one dialog. Cold start should be near-instant.
- Selecting multiple archives and pressing Enter spawns one process each (the
  shell's default verb behavior). Each shows its own dialog. A batch/queue mode
  is explicitly deferred (see [README non-goals](README.md)).
- The **current working directory** for layout decisions is the directory the
  archive lives in — derived from the archive path, not the process CWD, which
  is unreliable when launched by the shell.

## Components

```
                       ┌─────────────────────────────┐
   shell double-click  │        App Entry (CLI)       │
   ──────────────────▶ │  parse argv, modes, COM init │
                       └──────────────┬──────────────┘
                                      │
              ┌───────────────────────┼───────────────────────┐
              ▼                       ▼                         ▼
     ┌─────────────────┐   ┌────────────────────┐   ┌────────────────────┐
     │ Format Detector │   │  Extraction Engine │   │     UI Layer        │
     │ ext + magic →   │   │  libarchive wrap,  │◀─▶│  Win32 dialog,      │
     │ pipeline plan   │──▶│  temp staging,     │   │  progress, password,│
     └─────────────────┘   │  password, cancel  │   │  error dialogs      │
                           └─────────┬──────────┘   └────────────────────┘
                                     ▼
                           ┌────────────────────┐   ┌────────────────────┐
                           │  Layout Planner    │   │ Shell Integration  │
                           │  root analysis +   │──▶│  Explorer reveal,  │
                           │  auto-rename        │   │  open-and-select   │
                           └────────────────────┘   └────────────────────┘

     ┌──────────────────────────────────────────────────────────────────┐
     │ Shell Integration also provides --register / --unregister, invoked │
     │ by the WiX installer. See 04-shell-integration.md.                 │
     └──────────────────────────────────────────────────────────────────┘
```

### App Entry (CLI)
- Parses `argv`. Modes:
  - *(default)* `<archive-path>` — extract.
  - `--register` / `--unregister` — write/remove shell registration (called by
    the installer; also usable manually). See [Shell Integration](04-shell-integration.md).
- Initializes COM (`CoInitializeEx`) for shell APIs.
- Validates the path exists and is a supported type; otherwise shows an error.

### Format Detector
- Maps the archive to an extraction **pipeline plan**: which decompression
  filter(s) and container format apply, and whether it is a single-stream
  compressor or a true multi-entry archive.
- Detection order: **content sniffing (magic bytes) first, extension as
  fallback / tiebreaker.** Brotli has no magic header, so `.br` is detected by
  extension only. See [Format Support](02-format-support.md).

### Extraction Engine
- Wraps **libarchive** (primary), the **7z.dll/bit7z** backend (encrypted 7z +
  RAR), and the **brotli** library (`.br`) behind one internal extraction
  interface so the Layout Planner and UI are backend-agnostic.
- Runs extraction on a worker thread; the UI thread stays responsive.
- Extracts into a **temp staging directory** on the destination volume, reports
  progress via callback, honors a cancellation token, and invokes a password
  callback when an encrypted entry is encountered.
- See [Extraction Behavior](03-extraction-behavior.md).

### Layout Planner
- After staging completes, inspects the staged **root entries** and decides the
  final destination per the smart-layout rules, applying auto-rename on
  collision. Moves/renames staged items into the working directory.

### UI Layer
- Raw Win32. Owns the extraction dialog (label + progress bar + cancel), the
  password prompt, and the error dialog. See [UI Specification](05-ui-spec.md).

### Shell Integration
- Reveal-in-Explorer (reuse existing window if one shows the target folder,
  else open a new one) with the new item pre-selected.
- Registration/unregistration helpers used by the installer.

## Threading

- **UI thread:** message pump, dialog, progress updates (posted from worker).
- **Worker thread:** decompression/extraction + file moves. Communicates
  progress and completion to the UI thread via `PostMessage`. Cancellation is a
  shared atomic flag checked between entries and during long writes.

## External dependencies

| Dependency | Purpose | Notes |
|---|---|---|
| **libarchive** | Primary engine: decompression + container parsing for most formats, incl. encrypted zip | Statically linked preferred |
| **7z.dll** (via **bit7z**) | Encrypted 7z + all RAR/RAR5 (libarchive can't decrypt these) | Shipped alongside the exe; loaded on demand |
| **Google brotli** | `.br` decode | libarchive does not support brotli |
| Win32 / Common Controls | UI | OS-provided |
| Shell32 / COM | Explorer reveal, registration | OS-provided |

libarchive itself pulls in zlib, bzip2, liblzma, zstd, lz4, etc.; these come
along as its backends. Prefer static linking and vcpkg for reproducible builds.
`7z.dll` is the one runtime DLL we ship (bit7z is a static wrapper around it);
it is loaded only when an encrypted-7z or RAR input is routed to it, so it never
affects cold start for the common cases.
