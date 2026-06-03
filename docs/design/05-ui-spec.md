# UI Specification

Raw Win32 with common controls. Three dialogs total: extraction progress,
password prompt, and error. All are small, centered on the primary monitor (or
the monitor under the cursor), DPI-aware, and dismissable with `Esc`.

## Extraction dialog (primary)

A small rectangular modeless dialog, centered, always-on-top of nothing in
particular (normal top-level window), no minimize/maximize.

```
┌────────────────────────────────────────────┐
│                                              │
│   Expanding "project.tar.gz"                 │
│                                              │
│   ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓░░░░░░░░░░░░░   [ Cancel ]  │
│                                              │
└────────────────────────────────────────────┘
```

- **Label:** `Expanding "<filename>"` using the original archive filename
  (with extension). Truncate with an ellipsis if very long.
- **Progress bar:** determinate when total size is known; indeterminate
  (marquee) otherwise (see [progress reporting](03-extraction-behavior.md)).
- **Cancel button:** to the right of the bar. Triggers
  [cancellation](03-extraction-behavior.md#cancellation): abort, delete temp,
  close. `Esc` and the window close (`X`) behave like Cancel.
- Appears promptly on launch. For tiny archives that finish in <~200 ms, it may
  flash briefly or be suppressed until a short delay elapses (avoid a flicker on
  trivial extractions) — implementer's choice, document the chosen threshold.
- On completion the dialog closes and Explorer reveal runs. No success dialog.

### Behavioral notes
- The window is owned by the UI thread; progress updates arrive via
  `PostMessage` from the worker. The UI never blocks on extraction.
- Visual style: standard system theming via a common-controls manifest
  (v6 comctl32) so the progress bar and button look native on Win10/11.

## Password prompt

Shown when an encrypted archive is encountered; extraction pauses behind it.

```
┌────────────────────────────────────────────┐
│  "secret.7z" is password protected.          │
│                                              │
│  Password:  [••••••••••••••••••]             │
│                                              │
│              [  OK  ]   [ Cancel ]           │
└────────────────────────────────────────────┘
```

- Masked edit control; focus on open; `Enter` = OK, `Esc` = Cancel.
- Wrong password → re-show with a short inline message
  ("Incorrect password — try again."). A few attempts, then Cancel ends cleanly.
- Cancel aborts the whole extraction (temp removed).

## Error dialog

A standard `MessageBox`-style dialog (or themed task dialog) for unsupported
formats, corrupt archives, disk-full, permission errors, etc.

- Title: `Archive Extractor`.
- Body: plain-language description + the archive name. E.g.
  *"Couldn't expand 'broken.zip'. The archive appears to be corrupt."*
- Single **OK** that exits. No partial output left behind.

See [Error Handling](06-error-handling.md) for the message catalog.

## General UI requirements

- **Per-monitor DPI awareness v2** (manifest); crisp at any scaling.
- Centered placement on the active monitor.
- Keyboard accessible; respects system theme (light/dark via common controls).
- No taskbar clutter beyond the single transient window.
