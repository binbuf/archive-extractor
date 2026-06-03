# third_party/

Third-party engine dependencies (libarchive + backends, brotli, bit7z/7z.dll,
gtest) are managed by **vcpkg in manifest mode** — see `vcpkg.json` at the repo
root — rather than vendored here. This directory is a placeholder for any future
sources that cannot be sourced through vcpkg (e.g. a hand-placed `7z.dll`).
