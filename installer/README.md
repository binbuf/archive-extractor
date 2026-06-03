# installer/

WiX packaging for Archive Extractor (task 13). Produces **two** installers from a
Release build of the app:

| Artifact | Source | What it is |
|---|---|---|
| `ArchiveExtractor.msi` | `Package.wxs` | Per-machine **x64 MSI**. Lays down `ArchiveExtractor.exe` + `7z.dll` under `Program Files\Archive Extractor` and registers/unregisters shell associations. |
| `ArchiveExtractorSetup.exe` | `Bundle.wxs` | WiX **Burn bootstrapper** (.exe) wrapping the MSI for double-click install. |

## How it works

- **Payload:** the exe and the on-demand `7z.dll` (the one runtime DLL) are
  installed under Program Files (x64).
- **Registration:** done by invoking the app's own CLI — the `--register` /
  `--unregister` verbs (task 08) — as **deferred custom actions** (run elevated as
  SYSTEM). This keeps the installer and the app on a single source of truth for
  the registry key set; there is no duplicated registry table to drift.
  `Register()`/`Unregister()` themselves broadcast `SHChangeNotify(SHCNE_ASSOCCHANGED)`,
  so the shell refreshes associations immediately.
- **Upgrades:** `MajorUpgrade` replaces cleanly. Unregister is conditioned
  `REMOVE AND NOT UPGRADINGPRODUCTCODE`, so a major upgrade never tears down the
  freshly-registered associations from the new version.
- **Uninstall:** removes all files and (via `--unregister`) every key the app
  wrote — no orphaned ProgID or associations.
- **Default-handler limitation:** the installer never fabricates the hashed
  `UserChoice`. For owned extensions (e.g. `.zip`) the app only advertises via
  *Open with* / *Default apps*; the user opts in through the OS picker
  (`--set-default`, task 08).

## Building locally

```powershell
# 1) Build the app (Release) so build\x64-release has the exe + 7z.dll
.\scripts\build.ps1 -Release

# 2) Build both installers (installs the WiX v5 CLI + extension on demand)
.\installer\build-installer.ps1 -Version 1.0.0
# -> installer\out\ArchiveExtractor.msi
#    installer\out\ArchiveExtractorSetup.exe
```

`build-installer.ps1` pins **WiX v5** (`5.0.2`): WiX v6/v7 gate builds behind the
Open Source Maintenance Fee (OSMF) EULA. The Burn standard bootstrapper lives in
the `WixToolset.BootstrapperApplications.wixext` extension in v5 (it was
`WixToolset.Bal.wixext` in v4), added automatically by the script.

## CI

`.github/workflows/release.yml` builds the app, runs the tests, and produces both
installers on a version tag (`vX.Y.Z`), attaching them to a GitHub Release. The
MSI/bundle version comes from the tag.

## Verification (manual, clean VM)

Install → double-click each format extracts → app appears in *Default apps* →
uninstall → all files and registry keys gone (no orphaned associations) →
reinstall/upgrade replaces cleanly. Run this on a throwaway VM, not a dev box
(it writes to Program Files and HKLM).

## Post-MVP follow-ups

- **Code-signing** the exe + MSI + bundle (SmartScreen) — strongly recommended
  before real distribution.
- Auto-update.
- `7z.dll` is pinned to the bit7z-compatible 7-Zip line; keep it matched when the
  bit7z dependency moves.
