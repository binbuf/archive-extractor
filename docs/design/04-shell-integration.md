# Shell Integration

Two responsibilities: (1) register Archive Extractor as a handler for the
supported extensions so double-click launches it, and (2) reveal the extracted
result in File Explorer with the new item pre-selected.

## File-type registration (WiX / per-machine)

Packaging is a **per-machine WiX/MSI installer**. Registration writes a ProgID
and associates each extension with it under `HKLM\Software\Classes`.

### ProgID

A single ProgID for all supported types (the verb/command is identical):

```
HKLM\Software\Classes\ArchiveExtractor.Archive.1\
    (default)                = "Compressed Archive"
    DefaultIcon              = "<install>\ArchiveExtractor.exe,0"
    shell\open\command       = "\"<install>\ArchiveExtractor.exe\" \"%1\""
```

### Per-extension association

For each of `.zip .7z .zst .rar .xz .gz .lz4 .tar .bz2 .br` (and the `.tgz`
family), advertise the ProgID via `OpenWithProgids` so the app appears in
**Open with** and **Default apps**:

```
HKLM\Software\Classes\.7z\OpenWithProgids\ArchiveExtractor.Archive.1 = ""
HKLM\Software\Classes\.zst\OpenWithProgids\ArchiveExtractor.Archive.1 = ""
...
```

### Registered Application / capabilities

Register capabilities so the app is a first-class citizen in the Windows
**Default Apps** UI:

```
HKLM\Software\ArchiveExtractor\Capabilities\
    ApplicationName          = "Archive Extractor"
    ApplicationDescription   = "..."
    FileAssociations\.7z     = "ArchiveExtractor.Archive.1"
    FileAssociations\.zip    = "ArchiveExtractor.Archive.1"
    ...
HKLM\Software\RegisteredApplications\
    Archive Extractor = "Software\ArchiveExtractor\Capabilities"
```

### The Windows 10/11 default-handler limitation

Windows protects the *default* handler for each extension behind a hashed
`UserChoice` key that **cannot be set silently** by third parties
([ProgID docs](https://learn.microsoft.com/en-us/windows/win32/shell/fa-progids)).
Implications:

- For an extension **no app currently owns**, registering the ProgID as the
  class default can make us the handler directly.
- For an **owned** extension (notably `.zip`, owned by Explorer's Compressed
  Folders), we **cannot** silently steal the default. We:
  1. Register so we appear in *Open with* and *Default apps*, and
  2. Optionally, on first run or from a menu, invoke the OS picker
     (`SHOpenWithDialog` / the Default Apps deep-link) so the **user** sets us
     as default with one confirmation.
- We never fabricate the `UserChoice` hash.

### CLI hooks for the installer

- `ArchiveExtractor.exe --register` — write all of the above (idempotent).
- `ArchiveExtractor.exe --unregister` — remove them on uninstall.

The WiX package can write the registry directly **and/or** call these so the
logic has a single source of truth. After changes, broadcast
`SHChangeNotify(SHCNE_ASSOCCHANGED, ...)` so the shell refreshes.

## Launch contract

The shell invokes `shell\open\command` with the archive path as `%1`. The app:

1. Resolves the working directory from the archive's parent folder.
2. Runs extraction.
3. On success, reveals the result (below).

## Reveal in Explorer (open-and-select)

On success, reveal the **newly created top-level item** (the single extracted
file/folder, or the wrapper folder) selected in Explorer:

1. `CoInitializeEx`.
2. Enumerate open Explorer windows via `IShellWindows`. If one is already
   showing the **target working directory**, reuse it: bring it to the
   foreground and select the new item in that window.
3. Otherwise call
   [`SHOpenFolderAndSelectItems`](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shopenfolderandselectitems)
   with the working dir PIDL and the new item as the selection — this opens (or
   focuses) a window with the item pre-selected.
4. Bring the window to the front (`SetForegroundWindow`).

Selecting **one** top-level item satisfies the brief ("pre-select the newly
extracted assets… for the user to easily find"). Because layout always produces
a single top-level item (the file, the unwrapped folder, or the wrapper folder),
the selection target is unambiguous.

> Reuse note: `SHOpenFolderAndSelectItems` alone tends to open a new window.
> Genuine "reuse the existing window" requires the `IShellWindows` enumeration
> in step 2; treat that as the preferred path and the API call as the fallback.
