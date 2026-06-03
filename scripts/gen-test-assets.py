#!/usr/bin/env python3
"""Generate a comprehensive corpus of archive test assets.

This produces, for the Archive Extractor, one sample of *every kind* of archive
described in the design docs (see docs/design/02-format-support.md,
03-extraction-behavior.md, 07-passwords-edge-cases.md):

  * Every supported extension:
      Containers .......... .zip .7z .rar .tar
      Compound (filter+tar) .tar.gz .tgz .tar.bz2 .tbz2 .tar.xz .txz
                            .tar.zst .tar.lz4 .tar.br
      Single-stream ....... .gz .bz2 .xz .zst .lz4 .br

  * Every root layout / structure (for container & compound formats):
      single-file ......... exactly one file at the root
      single-folder ....... exactly one folder at the root (unwrapped on extract)
      multi-files ......... 2+ files at the root          (wrapped in <stem>/)
      multi-folders ....... 2+ folders at the root        (wrapped in <stem>/)
      mixed ............... files + folders at the root   (wrapped in <stem>/)

  * Password protection variants:
      zip ZipCrypto (classic), zip WinZip-AES,
      7z AES (clear headers), 7z AES with encrypted headers (-mhe=on),
      rar AES (clear headers), rar AES with encrypted headers (-hp)

  * Edge cases from the design: empty archive, single empty folder, macOS
    __MACOSX cruft, nested archive, zip-slip path traversal, illegal / reserved
    Windows names, duplicate path, symlink/hardlink tar, content-vs-extension
    mismatch (7z bytes named .zip), truncated/corrupt archive, unknown input,
    and a 7z multi-volume split set (.001..00N).

Single-stream formats (.gz/.bz2/.xz/.zst/.lz4/.br) are *always* one file by
definition, so they only get the single-file variant -- with a realistic inner
name so the extractor's name-stripping (page.html.br -> page.html) is exercised.

Formats that need tooling not present on this machine degrade gracefully: 7z
needs the `py7zr` package, AES zip needs `pyzipper`, and RAR needs WinRAR's
`rar.exe` (RAR is proprietary and cannot be created any other way). Missing pure
-Python packages are pip-installed best-effort unless --no-install is given;
anything still unavailable is SKIPPED with a clear reason in the final summary.

A manifest.json describing every asset (format, structure, password, expected
extraction outcome) is written alongside the assets for use by automated tests.

Usage:
    python scripts/gen-test-assets.py [--out DIR] [--clean] [--no-install]
                                      [--password PW] [--list]

    # Create ONE large archive of a specific format (manual / perf testing):
    python scripts/gen-test-assets.py --large zip   [--large-size 2G] [--large-files 4]
    python scripts/gen-test-assets.py --large tar.gz --large-size 512M
    python scripts/gen-test-assets.py --large xz    --large-size 5G   # >4G: Zip64/64-bit
"""

from __future__ import annotations

import argparse
import bz2
import gzip
import hashlib
import importlib
import io
import json
import lzma
import os
import random
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile
import zlib
from pathlib import Path
from shutil import which

# --------------------------------------------------------------------------- #
# Constants
# --------------------------------------------------------------------------- #

DEFAULT_PASSWORD = "test1234"

# Repo root = parent of the scripts/ dir holding this file.
REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT = REPO_ROOT / "tests" / "assets"

# Track outcomes for the end-of-run summary and the manifest.
RESULTS: list[dict] = []


def record(name: str, fmt: str, structure: str, password, status: str,
           reason: str = "", outcome: str = "") -> None:
    RESULTS.append({
        "name": name,
        "format": fmt,
        "structure": structure,
        "password": password,
        "status": status,        # "created" | "skipped"
        "reason": reason,
        "outcome": outcome,      # expected layout result on extraction
    })
    tag = "OK  " if status == "created" else "SKIP"
    extra = f"  ({reason})" if reason else ""
    print(f"  [{tag}] {name}{extra}")


# --------------------------------------------------------------------------- #
# Optional dependency loading (best-effort pip install)
# --------------------------------------------------------------------------- #

ALLOW_INSTALL = True
_MODULE_CACHE: dict[str, object] = {}


def load_module(import_name: str, pip_name: str):
    """Import a module, optionally pip-installing it first. Cache the result."""
    if import_name in _MODULE_CACHE:
        return _MODULE_CACHE[import_name]
    mod = None
    try:
        mod = importlib.import_module(import_name)
    except ImportError:
        if ALLOW_INSTALL:
            print(f"  ... installing {pip_name} (for {import_name})")
            try:
                subprocess.run(
                    [sys.executable, "-m", "pip", "install", "--quiet", pip_name],
                    check=True,
                )
                mod = importlib.import_module(import_name)
            except Exception as exc:  # noqa: BLE001 - best effort
                print(f"  ... could not install {pip_name}: {exc}")
                mod = None
    _MODULE_CACHE[import_name] = mod
    return mod


# --------------------------------------------------------------------------- #
# Content trees
#
# A "tree" maps a POSIX relative path -> bytes (a file). A path ending in "/"
# with a None value denotes an explicit empty directory.
# --------------------------------------------------------------------------- #

def _txt(s: str) -> bytes:
    return s.encode("utf-8")


def tree_single_file() -> dict:
    return {
        "readme.txt": _txt("The quick brown fox jumps over the lazy dog.\n"),
    }


def tree_single_folder() -> dict:
    return {
        "project/main.py": _txt("print('hello from project')\n"),
        "project/README.md": _txt("# project\n\nA single top-level folder.\n"),
        "project/lib/util.py": _txt("def add(a, b):\n    return a + b\n"),
    }


def tree_multi_files() -> dict:
    return {
        "alpha.txt": _txt("first file at root\n"),
        "beta.txt": _txt("second file at root\n"),
        "gamma.log": _txt("third file at root\n"),
    }


def tree_multi_folders() -> dict:
    return {
        "src/app.py": _txt("import sys\nprint(sys.argv)\n"),
        "src/core/engine.py": _txt("class Engine:\n    pass\n"),
        "docs/guide.md": _txt("# Guide\n\nMultiple folders at root.\n"),
    }


def tree_mixed() -> dict:
    return {
        "main.py": _txt("if __name__ == '__main__':\n    print('mixed root')\n"),
        "LICENSE": _txt("MIT License\n"),
        "src/core.py": _txt("VALUE = 42\n"),
        "assets/logo.txt": _txt("[logo placeholder]\n"),
    }


# (label, tree-fn, expected extraction outcome) for every container structure.
STRUCTURES = [
    ("single-file", tree_single_file,
     "one root entry (file) -> place the file directly"),
    ("single-folder", tree_single_folder,
     "one root entry (folder) -> place the folder directly, no wrapper"),
    ("multi-files", tree_multi_files,
     "2+ root entries -> wrap in a folder named after the archive stem"),
    ("multi-folders", tree_multi_folders,
     "2+ root entries -> wrap in a folder named after the archive stem"),
    ("mixed", tree_mixed,
     "2+ root entries -> wrap in a folder named after the archive stem"),
]


def iter_paths(tree: dict):
    """Yield (path, data_or_None, is_dir) sorted for deterministic output."""
    for path in sorted(tree):
        data = tree[path]
        is_dir = path.endswith("/")
        yield path, data, is_dir


# --------------------------------------------------------------------------- #
# Plain (unencrypted) builders
# --------------------------------------------------------------------------- #

def build_tar_bytes(tree: dict) -> bytes:
    """Build an uncompressed tar in memory from a content tree."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.GNU_FORMAT) as tf:
        for path, data, is_dir in iter_paths(tree):
            if is_dir:
                info = tarfile.TarInfo(name=path.rstrip("/"))
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                info.mtime = 0
                tf.addfile(info)
            else:
                info = tarfile.TarInfo(name=path)
                info.size = len(data)
                info.mode = 0o644
                info.mtime = 0
                tf.addfile(info, io.BytesIO(data))
    return buf.getvalue()


def write_plain_zip(path: Path, tree: dict) -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for p, data, is_dir in iter_paths(tree):
            if is_dir:
                zf.writestr(p if p.endswith("/") else p + "/", b"")
            else:
                zf.writestr(p, data)


def write_plain_tar(path: Path, tree: dict) -> None:
    path.write_bytes(build_tar_bytes(tree))


# --------------------------------------------------------------------------- #
# Single-stream compressors: bytes -> bytes (raise Unavailable if no backend)
# --------------------------------------------------------------------------- #

class Unavailable(Exception):
    pass


def comp_gz(data: bytes) -> bytes:
    return gzip.compress(data, compresslevel=9)


def comp_bz2(data: bytes) -> bytes:
    return bz2.compress(data, compresslevel=9)


def comp_xz(data: bytes) -> bytes:
    return lzma.compress(data, format=lzma.FORMAT_XZ)


def comp_zst(data: bytes) -> bytes:
    mod = load_module("zstandard", "zstandard")
    if mod is not None:
        return mod.ZstdCompressor(level=19).compress(data)
    return _cli_filter(["zstd", "-q", "-19"], data, "zstd")


def comp_lz4(data: bytes) -> bytes:
    mod = load_module("lz4.frame", "lz4")
    if mod is not None:
        return mod.compress(data)
    return _cli_filter(["lz4", "-q", "-z", "-9"], data, "lz4")


def comp_br(data: bytes) -> bytes:
    mod = load_module("brotli", "brotli")
    if mod is not None:
        return mod.compress(data, quality=11)
    return _cli_filter(["brotli", "-q", "11"], data, "brotli")


def _cli_filter(base_cmd: list[str], data: bytes, tool: str) -> bytes:
    """Compress `data` with a stdin/stdout CLI filter via temp files."""
    if which(base_cmd[0]) is None:
        raise Unavailable(f"{tool} unavailable (no module and no '{base_cmd[0]}' CLI)")
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "in"
        dst = Path(td) / "out"
        src.write_bytes(data)
        # All three CLIs accept:  <tool> [flags] -o OUTFILE INFILE  ... except lz4
        # which is positional. Use the common positional form: <tool> [flags] IN OUT.
        cmd = base_cmd + ["-f", str(src), str(dst)]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        return dst.read_bytes()


SINGLE_STREAM = [
    # (format key, extension, compressor, inner filename)
    ("gz", ".gz", comp_gz, "document.txt"),
    ("bz2", ".bz2", comp_bz2, "notes.txt"),
    ("xz", ".xz", comp_xz, "data.csv"),
    ("zst", ".zst", comp_zst, "payload.json"),
    ("lz4", ".lz4", comp_lz4, "frame.bin"),
    ("br", ".br", comp_br, "page.html"),
]

# Single-stream sample contents keyed by inner filename.
SINGLE_STREAM_CONTENT = {
    "document.txt": _txt("A single gzip-compressed text document.\n" * 4),
    "notes.txt": _txt("bzip2 single-stream notes.\n" * 4),
    "data.csv": _txt("id,name,value\n1,foo,10\n2,bar,20\n3,baz,30\n"),
    "payload.json": _txt('{"format":"zstd","entries":1,"ok":true}\n'),
    "frame.bin": bytes(range(256)) * 8,
    "page.html": _txt("<!doctype html>\n<title>brotli</title>\n<h1>hi</h1>\n"),
}

# Compound filter+tar formats: (format key, extension, compressor)
COMPOUND = [
    ("targz", ".tar.gz", comp_gz),
    ("tgz", ".tgz", comp_gz),
    ("tarbz2", ".tar.bz2", comp_bz2),
    ("tbz2", ".tbz2", comp_bz2),
    ("tarxz", ".tar.xz", comp_xz),
    ("txz", ".txz", comp_xz),
    ("tarzst", ".tar.zst", comp_zst),
    ("tarlz4", ".tar.lz4", comp_lz4),
    ("tarbr", ".tar.br", comp_br),
]


# --------------------------------------------------------------------------- #
# ZipCrypto (PKWARE traditional) encryption -- no external deps.
#
# Implemented directly because neither the stdlib nor any installed package can
# *write* classic ZipCrypto. Files are stored (method 0) which keeps the writer
# simple while still exercising the decrypt path.
# --------------------------------------------------------------------------- #

_CRC_TABLE = []
for _n in range(256):
    _c = _n
    for _ in range(8):
        _c = (0xEDB88320 ^ (_c >> 1)) if (_c & 1) else (_c >> 1)
    _CRC_TABLE.append(_c)


def _crc32_byte(crc: int, b: int) -> int:
    return ((crc >> 8) ^ _CRC_TABLE[(crc ^ b) & 0xFF]) & 0xFFFFFFFF


class _ZipCryptoCipher:
    def __init__(self, password: bytes):
        self.k = [0x12345678, 0x23456789, 0x34567890]
        for b in password:
            self._update(b)

    def _update(self, b: int) -> None:
        self.k[0] = _crc32_byte(self.k[0], b)
        self.k[1] = (self.k[1] + (self.k[0] & 0xFF)) & 0xFFFFFFFF
        self.k[1] = (self.k[1] * 134775813 + 1) & 0xFFFFFFFF
        self.k[2] = _crc32_byte(self.k[2], (self.k[1] >> 24) & 0xFF)

    def _keystream_byte(self) -> int:
        t = (self.k[2] | 2) & 0xFFFF
        return ((t * (t ^ 1)) >> 8) & 0xFF

    def encrypt(self, data: bytes) -> bytes:
        out = bytearray(len(data))
        for i, p in enumerate(data):
            out[i] = p ^ self._keystream_byte()
            self._update(p)  # update with plaintext when encrypting
        return bytes(out)


def write_zipcrypto_zip(path: Path, files: list[tuple[str, bytes]],
                        password: str) -> None:
    """Write a minimal, valid, ZipCrypto-encrypted, stored-method zip."""
    pw = password.encode("utf-8")
    dos_time, dos_date = 0, 0x21  # 1980-01-01 00:00:00
    out = bytearray()
    central = bytearray()

    for name, data in files:
        name_b = name.encode("utf-8")
        crc = zlib.crc32(data) & 0xFFFFFFFF

        # 12-byte encryption header; check byte = high byte of CRC (no descriptor).
        header = bytearray(11) + bytes([(crc >> 24) & 0xFF])
        cipher = _ZipCryptoCipher(pw)
        enc = cipher.encrypt(bytes(header)) + cipher.encrypt(data)

        comp_size = len(enc)
        uncomp_size = len(data)
        flags = 0x0001  # bit 0: encrypted
        offset = len(out)

        # Local file header.
        out += struct.pack(
            "<IHHHHHIIIHH",
            0x04034B50, 20, flags, 0, dos_time, dos_date,
            crc, comp_size, uncomp_size, len(name_b), 0,
        )
        out += name_b
        out += enc

        # Central directory header.
        central += struct.pack(
            "<IHHHHHHIIIHHHHHII",
            0x02014B50, 20, 20, flags, 0, dos_time, dos_date,
            crc, comp_size, uncomp_size, len(name_b), 0, 0, 0, 0, 0, offset,
        )
        central += name_b

    cd_offset = len(out)
    out += central
    out += struct.pack(
        "<IHHHHIIH",
        0x06054B50, 0, 0, len(files), len(files), len(central), cd_offset, 0,
    )
    path.write_bytes(bytes(out))


# --------------------------------------------------------------------------- #
# 7z (via py7zr) and RAR (via WinRAR rar.exe)
# --------------------------------------------------------------------------- #

def write_7z(path: Path, tree: dict, password: str | None = None,
             header_encryption: bool = False) -> None:
    py7zr = load_module("py7zr", "py7zr")
    if py7zr is None:
        raise Unavailable("py7zr unavailable (pip install py7zr)")
    kwargs = {}
    if password is not None:
        kwargs["password"] = password
        kwargs["header_encryption"] = header_encryption
    with py7zr.SevenZipFile(path, "w", **kwargs) as z:
        for p, data, is_dir in iter_paths(tree):
            if is_dir:
                continue  # py7zr has no clean empty-dir writestr; skip
            z.writestr(data, p)


def write_rar(path: Path, tree: dict, password: str | None = None,
              header_encryption: bool = False) -> None:
    rar_exe = which("rar")
    if rar_exe is None:
        raise Unavailable("rar.exe unavailable (RAR is proprietary; needs WinRAR)")
    with tempfile.TemporaryDirectory() as td:
        staging = Path(td)
        for p, data, is_dir in iter_paths(tree):
            target = staging / p
            if is_dir:
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
        cmd = [rar_exe, "a", "-ep1", "-o+"]
        if password is not None:
            cmd.append((f"-hp{password}") if header_encryption else (f"-p{password}"))
        cmd += [str(path.resolve()), "."]
        subprocess.run(cmd, cwd=staging, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# --------------------------------------------------------------------------- #
# Generators
# --------------------------------------------------------------------------- #

def gen_containers(out: Path, password: str) -> None:
    print("\n== Containers x structures ==")
    for struct_label, tree_fn, outcome in STRUCTURES:
        tree = tree_fn()

        # zip
        name = f"zip__{struct_label}.zip"
        write_plain_zip(out / name, tree)
        record(name, "zip", struct_label, None, "created", outcome=outcome)

        # tar
        name = f"tar__{struct_label}.tar"
        write_plain_tar(out / name, tree)
        record(name, "tar", struct_label, None, "created", outcome=outcome)

        # 7z
        name = f"7z__{struct_label}.7z"
        try:
            write_7z(out / name, tree)
            record(name, "7z", struct_label, None, "created", outcome=outcome)
        except Unavailable as e:
            record(name, "7z", struct_label, None, "skipped", reason=str(e))

        # rar
        name = f"rar__{struct_label}.rar"
        try:
            write_rar(out / name, tree)
            record(name, "rar", struct_label, None, "created", outcome=outcome)
        except (Unavailable, subprocess.CalledProcessError) as e:
            record(name, "rar", struct_label, None, "skipped", reason=str(e))


def gen_compound(out: Path) -> None:
    print("\n== Compound (filter + tar) x structures ==")
    for fmt_key, ext, comp in COMPOUND:
        for struct_label, tree_fn, outcome in STRUCTURES:
            name = f"{fmt_key}__{struct_label}{ext}"
            tree = tree_fn()
            try:
                blob = comp(build_tar_bytes(tree))
            except (Unavailable, subprocess.CalledProcessError) as e:
                record(name, fmt_key, struct_label, None, "skipped", reason=str(e))
                continue
            (out / name).write_bytes(blob)
            record(name, fmt_key, struct_label, None, "created", outcome=outcome)


def gen_single_stream(out: Path) -> None:
    print("\n== Single-stream (always one file) ==")
    for fmt_key, ext, comp, inner in SINGLE_STREAM:
        name = f"{inner}{ext}"
        data = SINGLE_STREAM_CONTENT[inner]
        try:
            blob = comp(data)
        except (Unavailable, subprocess.CalledProcessError) as e:
            record(name, fmt_key, "single-file", None, "skipped", reason=str(e))
            continue
        (out / name).write_bytes(blob)
        record(name, fmt_key, "single-file", None, "created",
               outcome=f"single-stream -> place inner file '{inner}' directly")

    # Stemless brotli: a file literally named ".br" — stripping the ".br" leaves
    # nothing, so the extractor must fall back to the archive stem (and finally a
    # generic name) rather than producing an empty filename. Exercises the
    # output-name fallback path that page.html.br (name-strip) does not.
    try:
        blob = comp_br(_txt("stemless brotli payload.\n" * 4))
    except (Unavailable, subprocess.CalledProcessError) as e:
        record(".br", "br", "single-file-stemless", None, "skipped", reason=str(e))
    else:
        (out / ".br").write_bytes(blob)
        record(".br", "br", "single-file-stemless", None, "created",
               outcome="stemless single-stream -> output name falls back to stem")


def gen_passwords(out: Path, password: str) -> None:
    print(f"\n== Password protected (password = '{password}') ==")
    tree = tree_mixed()
    outcome = "2+ root entries -> wrap in <stem>/ (after decrypt)"

    # zip ZipCrypto (classic) -- hand-rolled, no deps.
    name = "zip-zipcrypto__password.zip"
    files = [(p, d) for p, d, is_dir in iter_paths(tree) if not is_dir]
    write_zipcrypto_zip(out / name, files, password)
    record(name, "zip-zipcrypto", "mixed", password, "created", outcome=outcome)

    # zip WinZip-AES via pyzipper.
    name = "zip-aes__password.zip"
    pyzipper = load_module("pyzipper", "pyzipper")
    if pyzipper is None:
        record(name, "zip-aes", "mixed", password, "skipped",
               reason="pyzipper unavailable (pip install pyzipper)")
    else:
        with pyzipper.AESZipFile(out / name, "w",
                                 compression=pyzipper.ZIP_DEFLATED,
                                 encryption=pyzipper.WZ_AES) as z:
            z.setpassword(password.encode("utf-8"))
            for p, d in files:
                z.writestr(p, d)
        record(name, "zip-aes", "mixed", password, "created", outcome=outcome)

    # 7z AES (clear headers) and 7z AES with encrypted headers.
    for name, hdr in [("7z-aes__password.7z", False),
                      ("7z-aes-header__password.7z", True)]:
        fmt = "7z-aes-header" if hdr else "7z-aes"
        try:
            write_7z(out / name, tree, password=password, header_encryption=hdr)
            record(name, fmt, "mixed", password, "created", outcome=outcome)
        except Unavailable as e:
            record(name, fmt, "mixed", password, "skipped", reason=str(e))

    # RAR AES (clear headers) and RAR with encrypted headers (-hp).
    for name, hdr in [("rar-aes__password.rar", False),
                      ("rar-aes-header__password.rar", True)]:
        fmt = "rar-aes-header" if hdr else "rar-aes"
        try:
            write_rar(out / name, tree, password=password, header_encryption=hdr)
            record(name, fmt, "mixed", password, "created", outcome=outcome)
        except (Unavailable, subprocess.CalledProcessError) as e:
            record(name, fmt, "mixed", password, "skipped", reason=str(e))


def gen_edge_cases(out: Path) -> None:
    print("\n== Edge cases ==")

    # Empty archives (no entries) -> no output, no empty wrapper.
    name = "edge-empty.zip"
    with zipfile.ZipFile(out / name, "w"):
        pass
    record(name, "zip", "empty", None, "created",
           outcome="empty archive -> no output produced")

    name = "edge-empty.tar"
    write_plain_tar(out / name, {})
    record(name, "tar", "empty", None, "created",
           outcome="empty archive -> no output produced")

    # Single empty folder at root -> unwrapped (folder created, no children).
    name = "edge-single-empty-folder.zip"
    write_plain_zip(out / name, {"emptydir/": None})
    record(name, "zip", "single-empty-folder", None, "created",
           outcome="one empty folder at root -> place the folder directly")

    # macOS-authored zip: __MACOSX cruft + AppleDouble + .DS_Store + real folder.
    name = "edge-macosx.zip"
    write_plain_zip(out / name, {
        "myproject/main.py": _txt("print('mac')\n"),
        "myproject/README.md": _txt("# mac project\n"),
        "__MACOSX/._myproject": _txt("\x00\x05\x16\x07 appledouble\n"),
        "__MACOSX/myproject/._main.py": _txt("\x00\x05\x16\x07 appledouble\n"),
        ".DS_Store": bytes(64),
    })
    record(name, "zip", "macosx", None, "created",
           outcome="__MACOSX + real folder = 2 root entries -> wrap in <stem>/")

    # Nested archive: extracting yields the inner archive, NOT auto-recursed.
    name = "edge-nested.zip"
    inner = io.BytesIO()
    with zipfile.ZipFile(inner, "w", zipfile.ZIP_DEFLATED) as iz:
        iz.writestr("inner-file.txt", _txt("I am one level deep.\n"))
    write_plain_zip_raw(out / name, {
        "inner.zip": inner.getvalue(),
        "outer-readme.txt": _txt("Contains a nested archive.\n"),
    })
    record(name, "zip", "nested", None, "created",
           outcome="extract one level only -> inner.zip stays as a file")

    # zip-slip: a path-traversal entry the extractor must refuse/sanitize.
    name = "edge-zip-slip.zip"
    with zipfile.ZipFile(out / name, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("safe.txt", _txt("ordinary entry\n"))
        zf.writestr("../escaped.txt", _txt("should NOT escape the dest dir\n"))
        zf.writestr("nested/../../escaped2.txt", _txt("also must be contained\n"))
    record(name, "zip", "zip-slip", None, "created",
           outcome="path traversal must be rejected/sanitized (zip-slip guard)")

    # Illegal / reserved Windows names that must be sanitized on extract.
    name = "edge-illegal-names.zip"
    with zipfile.ZipFile(out / name, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("CON.txt", _txt("reserved device name\n"))
        zf.writestr("AUX", _txt("reserved device name\n"))
        zf.writestr("bad<name>.txt", _txt("illegal char <>\n"))
        zf.writestr("trailingdot.", _txt("trailing dot is illegal on NTFS\n"))
        zf.writestr("README", _txt("upper\n"))
        zf.writestr("readme", _txt("lower -> case-only collision on NTFS\n"))
    record(name, "zip", "illegal-names", None, "created",
           outcome="reserved/illegal/case-colliding names sanitized + auto-renamed")

    # tar with a symlink and a hardlink -> drives the copy/skip links prompt.
    name = "edge-links.tar"
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tf:
        real = _txt("the real target file\n")
        ti = tarfile.TarInfo("readme.txt")
        ti.size = len(real); ti.mtime = 0; ti.mode = 0o644
        tf.addfile(ti, io.BytesIO(real))
        sym = tarfile.TarInfo("link-to-readme.txt")
        sym.type = tarfile.SYMTYPE; sym.linkname = "readme.txt"; sym.mtime = 0
        tf.addfile(sym)
        hard = tarfile.TarInfo("hardlink-to-readme.txt")
        hard.type = tarfile.LNKTYPE; hard.linkname = "readme.txt"; hard.mtime = 0
        tf.addfile(hard)
        ext = tarfile.TarInfo("link-to-outside")
        ext.type = tarfile.SYMTYPE; ext.linkname = "/etc/passwd"; ext.mtime = 0
        tf.addfile(ext)  # external target -> must be skipped + logged
    (out / name).write_bytes(buf.getvalue())
    record(name, "tar", "links", None, "created",
           outcome="links present -> one-time copy/skip prompt; external link skipped")

    # Duplicate path within one zip (same entry twice) -> last-wins or rename+log.
    name = "edge-duplicate-path.zip"
    with zipfile.ZipFile(out / name, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("dup.txt", _txt("FIRST copy of dup.txt\n"))
        zf.writestr("dup.txt", _txt("SECOND copy of dup.txt (last-wins)\n"))
    record(name, "zip", "duplicate-path", None, "created",
           outcome="duplicate path -> last-wins (or rename) + log")

    # Content/extension disagreement: real 7z bytes carrying a .zip extension.
    name = "edge-renamed-7z-as-zip.zip"
    _py7zr = load_module("py7zr", "py7zr")
    if _py7zr is None:
        record(name, "7z-as-zip", "renamed", None, "skipped",
               reason="py7zr unavailable (pip install py7zr)")
    else:
        with tempfile.TemporaryDirectory() as td:
            seven = Path(td) / "real.7z"
            with _py7zr.SevenZipFile(seven, "w") as z:
                for p, d, is_dir in iter_paths(tree_mixed()):
                    if not is_dir:
                        z.writestr(d, p)
            (out / name).write_bytes(seven.read_bytes())
        record(name, "7z-as-zip", "renamed", None, "created",
               outcome="content sniff overrides extension -> detected as 7z, logged")

    # Corrupt/truncated archive -> clear error, temp removed, no partial output.
    name = "edge-truncated.zip"
    good = io.BytesIO()
    with zipfile.ZipFile(good, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("payload.txt", _txt("x" * 4096))
    blob = good.getvalue()
    (out / name).write_bytes(blob[: max(1, len(blob) * 6 // 10)])  # head 60%
    record(name, "zip", "truncated", None, "created",
           outcome="corrupt/truncated -> error dialog, temp removed")

    # Unknown / unsupported input -> 'not a supported archive', file untouched.
    name = "edge-unknown.bin"
    (out / name).write_bytes(b"NOT-AN-ARCHIVE\x00\x01\x02 just some bytes\n" * 8)
    record(name, "unknown", "unsupported", None, "created",
           outcome="unrecognized content+extension -> unsupported error, untouched")

    # 7z multi-volume split (.7z.001/.002/...). A 7z volume set is simply the
    # archive's bytes cut into fixed-size chunks named .001, .002, ...; the
    # engine is pointed at .001 and reads the rest. Build a normal 7z, then split.
    py7zr = load_module("py7zr", "py7zr")
    base = "edge-multivolume.7z"
    if py7zr is None:
        record(base + ".001", "7z-split", "multi-volume", None, "skipped",
               reason="py7zr unavailable (pip install py7zr)")
    else:
        try:
            # Incompressible deterministic data so the 7z is large enough to
            # actually span several volumes (compressible data would fit in one).
            # A SHA-256 keystream is effectively random, so it won't shrink.
            def noise(seed: int, n: int) -> bytes:
                out = bytearray()
                block = seed.to_bytes(8, "little")
                while len(out) < n:
                    block = hashlib.sha256(block).digest()
                    out += block
                return bytes(out[:n])

            big = {f"data/file{i:02d}.bin": noise(i, 8192) for i in range(8)}
            with tempfile.TemporaryDirectory() as td:
                whole = Path(td) / base
                with py7zr.SevenZipFile(whole, "w") as z:
                    for p, d, _ in iter_paths(big):
                        z.writestr(d, p)
                blob = whole.read_bytes()
            chunk = 16384
            parts = [blob[i:i + chunk] for i in range(0, len(blob), chunk)] or [b""]
            names = []
            for idx, part in enumerate(parts, start=1):
                pname = f"{base}.{idx:03d}"
                (out / pname).write_bytes(part)
                names.append(pname)
            record(names[0], "7z-split", "multi-volume", None, "created",
                   outcome=f"open .001 -> reads all parts ({', '.join(names)})")
        except Exception as e:  # noqa: BLE001
            record(base + ".001", "7z-split", "multi-volume", None, "skipped",
                   reason=f"volume split failed: {e}")


def write_plain_zip_raw(path: Path, files: dict) -> None:
    """Like write_plain_zip but stores already-compressed blobs uncompressed."""
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as zf:
        for name, data in files.items():
            zf.writestr(name, data)


# --------------------------------------------------------------------------- #
# Large-archive generator (opt-in, --large)
#
# Produces a single, deliberately large archive of one chosen format for manual
# / performance testing: the determinate progress bar, big-file handling
# (Zip64 / >4 GB / 64-bit counters), solid-archive decompression pacing, and
# cancellation responsiveness. It is intentionally NOT added to manifest.json
# so the normal unit-test suite never tries to extract a multi-gigabyte file.
#
# Content is deterministic pseudo-random (effectively incompressible) so the
# archive on disk is genuinely large, and compression runs at a fast/low setting
# since size -- not ratio -- is the point. Everything is streamed in chunks so
# memory stays bounded even for multi-GB targets (the staged data does need that
# much temporary disk under the system temp dir).
# --------------------------------------------------------------------------- #

# token -> (output filename, kind, single-stream codec key)
#   kind: "zip" | "tar" | "7z" | "compound" | "single"
LARGE_FORMATS = {
    "zip":    ("large.zip",     "zip",      None),
    "tar":    ("large.tar",     "tar",      None),
    "7z":     ("large.7z",      "7z",       None),
    "targz":  ("large.tar.gz",  "compound", "gz"),
    "tgz":    ("large.tgz",     "compound", "gz"),
    "tarbz2": ("large.tar.bz2", "compound", "bz2"),
    "tbz2":   ("large.tbz2",    "compound", "bz2"),
    "tarxz":  ("large.tar.xz",  "compound", "xz"),
    "txz":    ("large.txz",     "compound", "xz"),
    "tarzst": ("large.tar.zst", "compound", "zst"),
    "tarlz4": ("large.tar.lz4", "compound", "lz4"),
    "tarbr":  ("large.tar.br",  "compound", "br"),
    "gz":     ("large.bin.gz",  "single",   "gz"),
    "bz2":    ("large.bin.bz2", "single",   "bz2"),
    "xz":     ("large.bin.xz",  "single",   "xz"),
    "zst":    ("large.bin.zst", "single",   "zst"),
    "lz4":    ("large.bin.lz4", "single",   "lz4"),
    "br":     ("large.bin.br",  "single",   "br"),
}

_SIZE_UNITS = {"K": 1024, "M": 1024 ** 2, "G": 1024 ** 3, "T": 1024 ** 4}


def parse_size(text: str) -> int:
    """Parse a human-readable size ('256M', '2G', '1.5G') or a raw byte count."""
    s = text.strip().upper()
    if s.endswith("B"):            # accept 'MB' / 'MIB'
        s = s[:-1]
    if s.endswith("I"):
        s = s[:-1]
    if s and s[-1] in _SIZE_UNITS:
        return int(float(s[:-1]) * _SIZE_UNITS[s[-1]])
    return int(s)


def normalize_large_token(token: str) -> str:
    """Map user input ('tar.gz', '.zip', '7Z') to a LARGE_FORMATS key."""
    return token.strip().lower().lstrip(".").replace(".", "")


def _human(n: float) -> str:
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if n < 1024 or unit == "TiB":
            return f"{int(n)} B" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} TiB"


def _fill_random_file(path: Path, size: int, seed: int,
                      chunk: int = 8 * 1024 * 1024) -> None:
    """Write `size` bytes of deterministic, incompressible data to `path`."""
    rng = random.Random(seed)
    gen = getattr(rng, "randbytes", None)  # Python 3.9+: fast C-level generator
    if gen is None:                        # ancient Python: non-deterministic
        gen = os.urandom
    written = 0
    with open(path, "wb") as f:
        while written < size:
            n = min(chunk, size - written)
            f.write(gen(n))
            written += n


def _cli_compress_file(base_cmd: list[str], src: Path, dst: Path, tool: str,
                       out_flag: bool) -> None:
    if which(base_cmd[0]) is None:
        raise Unavailable(f"{tool} unavailable (no module and no '{base_cmd[0]}' CLI)")
    cmd = list(base_cmd)
    cmd += [str(dst), str(src)] if out_flag else [str(src), str(dst)]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)


def _compress_file_stream(src: Path, dst: Path, fmt_key: str,
                          chunk: int = 8 * 1024 * 1024) -> None:
    """Stream-compress `src` -> `dst` with single-stream codec `fmt_key`.

    Memory-bounded (chunked). Prefers a Python module; falls back to a CLI tool
    for zst/lz4/br; raises Unavailable if neither is present. Low/fast settings.
    """
    if fmt_key == "gz":
        with open(src, "rb") as i, gzip.open(dst, "wb", compresslevel=1) as o:
            shutil.copyfileobj(i, o, chunk)
    elif fmt_key == "bz2":
        with open(src, "rb") as i, bz2.open(dst, "wb", compresslevel=1) as o:
            shutil.copyfileobj(i, o, chunk)
    elif fmt_key == "xz":
        with open(src, "rb") as i, lzma.open(dst, "wb", preset=0) as o:
            shutil.copyfileobj(i, o, chunk)
    elif fmt_key == "zst":
        mod = load_module("zstandard", "zstandard")
        if mod is not None:
            with open(src, "rb") as i, open(dst, "wb") as o:
                mod.ZstdCompressor(level=3).copy_stream(
                    i, o, read_size=chunk, write_size=chunk)
        else:
            _cli_compress_file(["zstd", "-q", "-1", "-f", "-o"], src, dst,
                               "zstd", out_flag=True)
    elif fmt_key == "lz4":
        mod = load_module("lz4.frame", "lz4")
        if mod is not None:
            with open(src, "rb") as i, mod.LZ4FrameFile(str(dst), "wb") as o:
                shutil.copyfileobj(i, o, chunk)
        else:
            _cli_compress_file(["lz4", "-q", "-1", "-f", "-z"], src, dst,
                               "lz4", out_flag=False)
    elif fmt_key == "br":
        mod = load_module("brotli", "brotli")
        if mod is not None:
            comp = mod.Compressor(quality=1)
            with open(src, "rb") as i, open(dst, "wb") as o:
                while True:
                    block = i.read(chunk)
                    if not block:
                        break
                    o.write(comp.process(block))
                o.write(comp.finish())
        else:
            _cli_compress_file(["brotli", "-q", "1", "-f", "-o"], src, dst,
                               "brotli", out_flag=True)
    else:
        raise ValueError(f"unknown single-stream codec '{fmt_key}'")


def _build_large_7z(dst: Path, staged: list[tuple[str, Path]]) -> None:
    py7zr = load_module("py7zr", "py7zr")
    if py7zr is None:
        raise Unavailable("py7zr unavailable (pip install py7zr)")
    # Store (no compression) so building a large 7z of random data stays fast;
    # fall back to py7zr's default LZMA2 if this build doesn't accept FILTER_COPY.
    try:
        z = py7zr.SevenZipFile(dst, "w", filters=[{"id": py7zr.FILTER_COPY}])
    except Exception:  # noqa: BLE001 - older/edge py7zr without FILTER_COPY
        z = py7zr.SevenZipFile(dst, "w")
    with z:
        for arc, p in staged:
            z.write(str(p), arcname=arc)


def gen_large(out: Path, token: str, total_size: int, nfiles: int) -> int:
    """Create one large archive of `token` format. Returns a process exit code."""
    norm = normalize_large_token(token)
    if norm not in LARGE_FORMATS:
        print(f"error: unknown --large format '{token}'.")
        print(f"  supported: {', '.join(sorted(LARGE_FORMATS))}")
        return 2

    fname, kind, codec = LARGE_FORMATS[norm]
    if kind == "single":
        nfiles = 1  # single-stream formats are one file by definition
    nfiles = max(1, nfiles)

    out.mkdir(parents=True, exist_ok=True)
    dst = out / fname
    print(f"Generating large {norm} archive -> {dst}")
    print(f"  target uncompressed: {_human(total_size)} across {nfiles} file(s)")

    per = total_size // nfiles
    sizes = [per] * nfiles
    sizes[-1] += total_size - per * nfiles  # remainder into the last file

    with tempfile.TemporaryDirectory() as td:
        staging = Path(td)
        staged: list[tuple[str, Path]] = []  # (arcname, on-disk path)
        for idx, sz in enumerate(sizes):
            arc = (f"large/part{idx:03d}.bin" if nfiles > 1
                   else "large-payload.bin")
            p = staging / f"part{idx:03d}.bin"
            print(f"  writing {arc} ({_human(sz)}) ...", flush=True)
            _fill_random_file(p, sz, seed=0xA11CE + idx)
            staged.append((arc, p))

        print(f"  packing {kind} archive ...", flush=True)
        try:
            if kind == "zip":
                # DEFLATE level 1 keeps it fast on random data; Zip64 auto-on.
                with zipfile.ZipFile(dst, "w", compression=zipfile.ZIP_DEFLATED,
                                     compresslevel=1, allowZip64=True) as zf:
                    for arc, p in staged:
                        zf.write(p, arcname=arc)
            elif kind == "tar":
                with tarfile.open(dst, "w") as tf:  # uncompressed tar
                    for arc, p in staged:
                        tf.add(str(p), arcname=arc)
            elif kind == "7z":
                _build_large_7z(dst, staged)
            elif kind == "compound":
                # Stream a plain tar to a temp file, then single-stream compress.
                tmp_tar = staging / "_large.tar"
                with tarfile.open(tmp_tar, "w") as tf:
                    for arc, p in staged:
                        tf.add(str(p), arcname=arc)
                _compress_file_stream(tmp_tar, dst, codec)
            elif kind == "single":
                _compress_file_stream(staged[0][1], dst, codec)
            else:  # pragma: no cover - guarded by LARGE_FORMATS
                raise ValueError(kind)
        except (Unavailable, subprocess.CalledProcessError, OSError) as e:
            print(f"error: could not build large {norm} archive: {e}")
            if dst.exists():
                dst.unlink()
            return 1

    archive_size = dst.stat().st_size
    print(f"\nDone. Wrote {dst}")
    print(f"  archive on disk : {_human(archive_size)}")
    print(f"  uncompressed    : {_human(total_size)}")
    print("  (not in manifest.json -- standalone asset for manual/perf testing)")
    return 0


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #

def write_manifest(out: Path, password: str) -> None:
    created = [r for r in RESULTS if r["status"] == "created"]
    skipped = [r for r in RESULTS if r["status"] == "skipped"]
    manifest = {
        "password": password,
        "counts": {"created": len(created), "skipped": len(skipped)},
        "assets": RESULTS,
    }
    (out / "manifest.json").write_text(
        json.dumps(manifest, indent=2), encoding="utf-8")


def write_readme(out: Path, password: str) -> None:
    (out / "README.md").write_text(
        "# Archive Extractor — test assets\n\n"
        "Generated by `scripts/gen-test-assets.py`. Do not edit by hand; rerun\n"
        "the script to regenerate. See `manifest.json` for a machine-readable\n"
        "description of every asset (format, structure, password, and expected\n"
        "extraction outcome).\n\n"
        f"**Password for all encrypted assets:** `{password}`\n\n"
        "Naming: `<format>__<structure><ext>` for containers/compound formats,\n"
        "realistic inner names (e.g. `page.html.br`) for single-stream formats,\n"
        "and `edge-*` for the edge cases.\n\n"
        "Structures: single-file, single-folder, multi-files, multi-folders,\n"
        "mixed. Per the smart-layout rule, 1 root entry is placed directly and\n"
        "2+ root entries are wrapped in a folder named after the archive stem.\n",
        encoding="utf-8",
    )


def main() -> int:
    global ALLOW_INSTALL

    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT,
                        help=f"output directory (default: {DEFAULT_OUT})")
    parser.add_argument("--clean", action="store_true",
                        help="delete existing generated assets in --out first")
    parser.add_argument("--no-install", action="store_true",
                        help="do not pip-install missing packages; skip instead")
    parser.add_argument("--password", default=DEFAULT_PASSWORD,
                        help=f"password for encrypted assets (default: {DEFAULT_PASSWORD})")
    parser.add_argument("--list", action="store_true",
                        help="list what would be generated and exit")
    parser.add_argument("--large", metavar="FORMAT",
                        help="create ONE large archive of FORMAT (e.g. zip, 7z, "
                             "tar, tar.gz, tgz, tar.xz, gz, xz, zst, lz4, br) for "
                             "manual/perf testing, then exit. Streamed; NOT added "
                             "to manifest.json.")
    parser.add_argument("--large-size", default="256M",
                        help="uncompressed size for --large (e.g. 512M, 2G, 5G; "
                             "default 256M). >4G exercises Zip64 / 64-bit counters.")
    parser.add_argument("--large-files", type=int, default=4,
                        help="number of inner files for --large containers/"
                             "compound (default 4; single-stream is always 1)")
    args = parser.parse_args()

    ALLOW_INSTALL = not args.no_install

    # --large is a standalone mode: build just the one big archive and exit,
    # without touching the corpus or its manifest.
    if args.large:
        try:
            size = parse_size(args.large_size)
        except ValueError:
            print(f"error: invalid --large-size '{args.large_size}'")
            return 2
        if size <= 0:
            print("error: --large-size must be positive")
            return 2
        return gen_large(args.out, args.large, size, args.large_files)

    if args.list:
        print("Containers x structures: zip, tar, 7z, rar  x  "
              + ", ".join(s[0] for s in STRUCTURES))
        print("Compound x structures:   "
              + ", ".join(c[0] for c in COMPOUND) + "  x  (same structures)")
        print("Single-stream:           "
              + ", ".join(f"{inner}{ext}" for _, ext, _, inner in SINGLE_STREAM))
        print("Passwords:               zip-zipcrypto, zip-aes, 7z-aes, "
              "7z-aes-header, rar-aes, rar-aes-header")
        print("Edge cases:              empty, single-empty-folder, macosx, "
              "nested, zip-slip, illegal-names, multivolume")
        print("Large (--large FORMAT):  " + ", ".join(sorted(LARGE_FORMATS)))
        return 0

    out = args.out
    out.mkdir(parents=True, exist_ok=True)

    if args.clean:
        for f in out.iterdir():
            if f.is_file():
                f.unlink()
        print(f"Cleaned {out}")

    print(f"Generating test assets into: {out}")
    gen_containers(out, args.password)
    gen_compound(out)
    gen_single_stream(out)
    gen_passwords(out, args.password)
    gen_edge_cases(out)

    write_manifest(out, args.password)
    write_readme(out, args.password)

    created = sum(1 for r in RESULTS if r["status"] == "created")
    skipped = sum(1 for r in RESULTS if r["status"] == "skipped")
    print(f"\nDone. {created} created, {skipped} skipped.")
    if skipped:
        print("Skipped (missing tooling):")
        for r in RESULTS:
            if r["status"] == "skipped":
                print(f"  - {r['name']}: {r['reason']}")
    print(f"Manifest: {out / 'manifest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
