# Changelog

All notable changes to Archive Extractor are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.1] - 2026-06-03

### Changed
- `--set-default` now registers the chosen app as the default handler without
  re-opening (re-extracting) the sample file. The sample is used only to
  identify the file type for the OS "Open with" picker.

### Removed
- Dropped the first-run "make Archive Extractor your default?" prompt. A
  quick-extract utility should not interrupt a successful extraction with an
  upsell; the installer already advertises the associations (Open with +
  Default apps), and `--set-default` remains for anyone who wants to opt in.

## [0.1.0] - 2026-06-03

### Added
- Initial release: macOS-Archive-Utility-style extractor for Windows.
- Extraction backends: libarchive (ZIP, 7z, TAR, GZ, BZ2, XZ, ZSTD, LZ4),
  bit7z/7z.dll (RAR/RAR5 and their multi-volume chains, encrypted 7z), and
  Brotli (`.br`, `.tar.br`).
- Password prompt with retry for encrypted archives.
- Shell integration: per-machine file-type registration (Open with + Default
  apps) for the supported extensions, plus a `--set-default` verb.
- Installers: per-machine x64 MSI and a WiX Burn EXE bootstrapper, built and
  published from CI on a `v*` tag.

[0.1.1]: https://github.com/binbuf/archive-extractor/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/binbuf/archive-extractor/releases/tag/v0.1.0
