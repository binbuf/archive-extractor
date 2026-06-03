# Building Archive Extractor

A native x64 Windows application. Builds with **CMake + Ninja** and pulls its
engine dependencies through **vcpkg** in manifest mode.

## Prerequisites

- **Windows 10/11 x64.**
- **Visual Studio 2022 or 2026** with the *Desktop development with C++* workload
  (MSVC v143+ toolset, Windows 10/11 SDK). This bundles CMake (≥ 3.21) and Ninja.
- **vcpkg.** A full clone with a ports tree (the VS-bundled tool-only vcpkg is
  not sufficient — it has no ports). Point `VCPKG_ROOT` at it:

  ```powershell
  git clone https://github.com/microsoft/vcpkg C:\vcpkg
  C:\vcpkg\bootstrap-vcpkg.bat
  setx VCPKG_ROOT C:\vcpkg
  ```

  Dependencies and their versions are pinned by `vcpkg.json`
  (`builtin-baseline`), so any vcpkg clone resolves identical versions.

The dependency set (libarchive + zlib/bzip2/lz4/lzma/zstd/openssl, brotli,
bit7z/7zip, gtest) is built from source on first configure. Expect a **long
initial configure** (tens of minutes); subsequent builds reuse the vcpkg binary
cache.

## Build

Open a **Developer PowerShell for VS** (or *x64 Native Tools Command Prompt*) so
the MSVC compiler, CMake, and Ninja are on `PATH`, then from the repo root:

```powershell
# Configure + build (Release)
cmake --preset x64-release
cmake --build --preset x64-release

# Run the unit tests
ctest --preset x64-release
```

Swap `x64-release` for `x64-debug` for a debug build. The executable lands at:

```
build/x64-release/ArchiveExtractor.exe
```

## One-command CI configure + build

```powershell
cmake --preset x64-release && cmake --build --preset x64-release && ctest --preset x64-release
```

(`$env:VCPKG_ROOT` must be set; CI should also set
`VCPKG_BINARY_SOURCES` / `VCPKG_DEFAULT_BINARY_CACHE` to persist the binary cache
between runs.)

## Smoke test

```powershell
build\x64-release\ArchiveExtractor.exe "C:\path\to\some.zip"   # logs path + parent working dir
build\x64-release\ArchiveExtractor.exe --register             # exit code 0 (stub)
build\x64-release\ArchiveExtractor.exe                        # no-op, exit code 0
```

Because the exe is GUI-subsystem, `Log()` output appears in the attached console
(when launched from a terminal) and in the debugger output window.

## Installers (MSI + EXE)

After a Release build, package both installers with WiX (the script installs the
pinned WiX v5 CLI + bootstrapper extension on demand):

```powershell
.\scripts\build.ps1 -Release
.\installer\build-installer.ps1 -Version 1.0.0
# -> installer\out\ArchiveExtractor.msi          (per-machine x64 MSI)
#    installer\out\ArchiveExtractorSetup.exe      (Burn bootstrapper .exe)
```

In CI, `.github/workflows/release.yml` builds the app, runs the tests, and
produces + publishes both installers on a version tag (`vX.Y.Z`). See
`installer/README.md` for how registration, upgrades, and uninstall work.

## Project layout

| Path | Contents |
|---|---|
| `include/archive_core/` | Public headers for the GUI-free core library. |
| `src/core/` | `archive_core` static lib — CLI parse, path/layout logic (unit-testable). |
| `src/app/` | `ArchiveExtractor.exe` Win32 front end + embedded `app.manifest`. |
| `tests/` | GoogleTest targets wired into CTest. |
| `installer/` | WiX/MSI packaging (task 13). |
| `third_party/` | Placeholder; deps come via vcpkg manifest. |

The split keeps detection/engine/layout code in `archive_core` so it can be
unit-tested without instantiating the Win32 UI.
