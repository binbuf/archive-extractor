# 10 — Password-protected archives (prompt + all backends)

## Goal
Implement the full password flow: the password prompt dialog, encrypted-zip via
libarchive and encrypted-7z/RAR via 7z.dll, correct prompt timing (including
encrypted-header archives), retry/cache/cancel, and the security requirements.

## Depends on
- [09 — 7z.dll backend](09-7zdll-backend.md) (encrypted-7z/RAR routing + password
  callback).
- [03 — Extraction engine](03-extraction-engine-libarchive.md) (libarchive
  passphrase + the password callback hook).
- [05 — UI extraction dialog](05-ui-extraction-dialog.md) (dialog infrastructure).

## Design refs
- [07-passwords-edge-cases.md §1](../../design/07-passwords-edge-cases.md) — full password behavior, timings, security.
- [05-ui-spec.md §Password prompt](../../design/05-ui-spec.md).
- [03-extraction-behavior.md §Password-protected archives](../../design/03-extraction-behavior.md).

## Scope — in
- **Password prompt dialog:** masked edit control, focus on open, `Enter` = OK,
  `Esc` = Cancel; title/body name the archive (e.g. *`"secret.7z"` is password
  protected.*). Centered, DPI-aware, themed.
- **Backend wiring:**
  - encrypted **zip** → libarchive `archive_read_add_passphrase()`;
  - encrypted **7z** and **RAR/RAR5** → bit7z `setPassword()` before extract.
- **Prompt timing:**
  - **Encrypted data, clear headers (typical):** enumerate + plan layout first,
    prompt when extraction needs the password.
  - **Encrypted headers** (7z `-mhe=on`, RAR `-hp`): the file list itself is
    encrypted — **prompt first**, before any layout/progress. Detector recognizes
    this state; sequence is: password prompt → (on success) progress dialog.
- **Mixed / per-entry:**
  - zip mixed (some entries encrypted) → prompt at the first encrypted entry;
  - per-entry differing passwords (rare) → try cached passphrase first; on
    failure for a specific entry, re-prompt for it (libarchive can hold multiple
    registered passphrases).
- **Retry / cache / cancel:**
  - **cache** the accepted passphrase for the lifetime of this one extraction;
    never re-prompt per file for a single-password archive;
  - **wrong password** → re-show prompt with inline *"Incorrect password — try
    again."*; cap retries (e.g. 3); after the cap or on Cancel, abort the whole
    extraction and delete temp (nothing left behind);
  - a password supplied for one archive is **never** reused for another process.
- **Security:** mask input; **never** write the password to logs, temp files,
  registry, or telemetry; hold only in memory for the extraction and **zero the
  buffer** after use; no "remember password" in v1.

## Scope — out
- Symlink prompt and other edge cases (task 12).

## Deliverables
- Password prompt dialog wired into the worker→UI flow (worker blocks for the
  password via a synchronized request to the UI thread).
- libarchive + bit7z passphrase integration.
- Encrypted-header detection driving the prompt-first sequence.
- Retry/cap/cache logic and secure buffer handling (zeroed).
- Tests from the generated corpus (see
  [Testing conventions](README.md#testing-conventions); password = `test1234`):
  encrypted zip ZipCrypto (`zip-zipcrypto__password.zip`) + WinZip AES
  (`zip-aes__password.zip`), encrypted 7z clear-header (`7z-aes__password.7z`) +
  `-mhe` encrypted-header (`7z-aes-header__password.7z`), and encrypted RAR
  (`rar-aes*.rar`, incl. `-hp` — gated on `rar.exe` availability). Cover
  wrong-password retry then success, retry-cap then clean abort, mixed zip, and
  cancel at the prompt. Each test extracts into a unique temp dir and **deletes it
  immediately afterward on every path**; assert the log/temp never contain the
  password and that an aborted attempt leaves no decompressed output.

## Acceptance criteria
- Each encrypted format prompts, accepts the password, and extracts.
- Encrypted-header archives prompt **before** any layout/progress UI.
- Wrong password re-prompts with the inline message; exceeding the cap aborts
  cleanly (temp removed).
- The cached password is reused within the archive but never across processes.
- Password never appears in logs, temp, or the registry; buffer is zeroed.

## Notes & risks
- The worker thread must pause for an async UI prompt without deadlocking the
  message pump — use a request/response handoff via `PostMessage` + an event.
- Encrypted-header timing changes the dialog order; test it explicitly.
