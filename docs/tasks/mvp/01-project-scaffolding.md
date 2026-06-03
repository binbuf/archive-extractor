# 01 — Project scaffolding & build system

## Goal
Stand up the repository, build system, and a minimal Win32 executable that
launches, parses its command line, initializes COM, and exits. Everything later
hangs off this skeleton.

## Depends on
Nothing.

## Design refs
- [01-architecture.md](../../design/01-architecture.md) — process model, CLI modes, components.
- [README.md](../../design/README.md) — product decisions (Raw Win32, libarchive, vcpkg).

## Scope — in
- Repository layout: `src/`, `include/`, `tests/`, `installer/`, `third_party/`
  (or vcpkg manifest), `CMakeLists.txt`.
- **CMake + vcpkg** with a manifest (`vcpkg.json`) pinning **libarchive** (and
  its backends: zlib, bzip2, liblzma, zstd, lz4) plus **brotli** and a
  **bit7z/7z.dll** placeholder dependency for later tasks. Prefer static linking
  for libarchive and backends.
- **64-bit (x64) build only.** Release + Debug configs.
- A Windows GUI subsystem `ArchiveExtractor.exe` target with:
  - An application **manifest** declaring **per-monitor DPI awareness v2**,
    **comctl32 v6** (common controls / visual styles), and **`longPathAware`**.
  - `wWinMain` entry; UTF-16 throughout.
- **CLI parsing** for the three modes from the architecture doc:
  - default: `<archive-path>` (just validate + log for now; extraction lands in
    later tasks),
  - `--register` / `--unregister` (stubs that return success; real logic in task 08).
- `CoInitializeEx` on startup, `CoUninitialize` on exit.
- Resolve the **working directory** from the archive path's parent (not process CWD).
- Path-existence + basic supported-extension check; on failure, a placeholder
  `MessageBox` (replaced by the real error dialog in task 06).
- A unit-test target (e.g. GoogleTest or doctest via vcpkg) wired into CTest,
  even if it starts with one trivial test.

## Scope — out
- Any real detection, extraction, UI, or registration logic (later tasks).

## Deliverables
- Building x64 Release/Debug `ArchiveExtractor.exe`.
- `vcpkg.json` manifest resolving all engine dependencies.
- App manifest with DPI v2 + comctl6 + longPathAware embedded in the exe.
- A `BUILD.md` documenting prerequisites (VS toolset, CMake, vcpkg bootstrap)
  and the build commands.
- CI-ready: a documented one-command configure+build.

## Acceptance criteria
- `cmake --preset x64-release && cmake --build` produces `ArchiveExtractor.exe`.
- Running `ArchiveExtractor.exe "C:\path\some.zip"` parses the path, derives the
  parent as the working directory, and logs/echoes both without crashing.
- Running with `--register` / `--unregister` returns exit code 0 (stub).
- Running with no argument does **not** error (per design §16: short notice or
  no-op).
- `ctest` runs and the trivial test passes.
- `dumpbin`/Resource view confirms the DPI-v2 + comctl6 + longPathAware manifest
  is embedded.

## Notes & risks
- Decide and document the source-tree split between the **app** and a **core
  library** (`archive_core`) so the engine/detector/layout code is unit-testable
  without the GUI. Recommended: put detection, engine, and layout in a static
  lib linked by both the exe and the test target.
- Pin vcpkg to a commit/baseline for reproducible builds.
