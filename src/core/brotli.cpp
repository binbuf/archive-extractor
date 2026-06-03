// Google-brotli (.br) extraction backend (task 11).
//
// Implements BrotliExtractor, the IExtractor for `.br` inputs. libarchive
// deliberately has no brotli support (the raw stream carries no magic header),
// so brotli is decoded here with Google's reference library, which is statically
// linked. Because this code only runs when ResolveBackend routes a plan to
// Backend::Brotli — and that only happens for `.br`/`.tar.br` — the brotli
// machinery is never touched for any other format, so cold start for the
// libarchive/7z.dll paths is unaffected.
//
// Two shapes, selected by plan.kind (brotli is detected by extension only):
//   * Single-file `.br`  -> decode straight to one output file (common path).
//   * `.tar.br`          -> decode to a temp `.tar`, then untar via
//                           LibarchiveExtractor so the layout applies to the
//                           tar's entries (uncommon branch).

#include "archive_core/extract.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <brotli/decode.h>

#include "archive_core/logging.h"

namespace fs = std::filesystem;

namespace ae {
namespace {

ExtractResult FailBr(ExtractStatus st, std::wstring msg) {
    ExtractResult r;
    r.status = st;
    r.message = std::move(msg);
    return r;
}

// Normalize separators to backslash (mirrors the helper in extract.cpp).
std::wstring NormalizeSeparators(std::wstring_view s) {
    std::wstring out(s);
    std::replace(out.begin(), out.end(), L'/', L'\\');
    return out;
}

std::wstring JoinPath(std::wstring_view base, std::wstring_view child) {
    std::wstring out(base);
    if (!out.empty() && out.back() != L'\\' && out.back() != L'/') {
        out.push_back(L'\\');
    }
    out.append(child);
    return out;
}

// Leaf-only name: drop any directory components a hint might carry, so a
// single-stream output can never escape the staging root.
std::wstring LeafOnly(std::wstring_view name) {
    std::wstring n = NormalizeSeparators(name);
    const std::size_t sep = n.find_last_of(L'\\');
    return sep == std::wstring::npos ? n : n.substr(sep + 1);
}

std::uint64_t FileSizeOf(const std::wstring& extPath) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(extPath.c_str(), GetFileExInfoStandard, &fad)) {
        return (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) |
               fad.nFileSizeLow;
    }
    return 0;
}

// Write `len` bytes from `p` to `h`, looping over partial writes. Returns false
// on the first failed/short write (caller treats as WriteFailed).
bool WriteAll(HANDLE h, const unsigned char* p, std::size_t len) {
    while (len > 0) {
        DWORD wrote = 0;
        const DWORD chunk =
            static_cast<DWORD>(std::min<std::size_t>(len, 1u << 20));
        if (!WriteFile(h, p, chunk, &wrote, nullptr) || wrote == 0) return false;
        p += wrote;
        len -= wrote;
    }
    return true;
}

// A throttled progress emitter (a slim local copy of extract.cpp's; that one is
// file-local there). When `total` is 0 the bar is indeterminate.
class ProgressThrottle {
   public:
    explicit ProgressThrottle(const ExtractCallbacks& cb) : cb_(cb) {
        last_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(1000);
    }
    void Emit(std::uint64_t done, std::uint64_t total, std::wstring_view entry,
              bool force = false) {
        if (!cb_.onProgress) return;
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last_ < std::chrono::milliseconds(40)) return;
        last_ = now;
        Progress p;
        p.done = done;
        p.total = total;
        p.currentEntry = std::wstring(entry);
        cb_.onProgress(p);
    }

   private:
    const ExtractCallbacks& cb_;
    std::chrono::steady_clock::time_point last_;
};

// Stream-decode the brotli file at `srcExt` (an extended-length path) into the
// already-open output handle `hOut`. Drives progress by compressed bytes read
// vs `srcSize` and polls cancellation between reads. Returns Ok on a clean
// finish; CorruptInput on a truncated/garbage stream; Cancelled / OpenFailed /
// WriteFailed otherwise. `err` carries a short diagnostic on failure.
ExtractStatus DecodeBrotli(const std::wstring& srcExt, HANDLE hOut,
                           std::uint64_t srcSize, const ExtractCallbacks& cb,
                           ProgressThrottle& progress, std::wstring_view label,
                           std::wstring& err) {
    HANDLE hIn =
        CreateFileW(srcExt.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hIn == INVALID_HANDLE_VALUE) {
        err = L"cannot open brotli input";
        return ExtractStatus::OpenFailed;
    }

    BrotliDecoderState* state =
        BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) {
        CloseHandle(hIn);
        err = L"brotli decoder allocation failed";
        return ExtractStatus::OpenFailed;
    }

    constexpr std::size_t kInBuf = 1u << 16;   // 64 KiB compressed read chunk
    constexpr std::size_t kOutBuf = 1u << 16;  // 64 KiB decode flush chunk
    std::vector<unsigned char> inBuf(kInBuf);
    std::vector<unsigned char> outBuf(kOutBuf);

    std::size_t availIn = 0;
    const unsigned char* nextIn = nullptr;
    std::uint64_t totalRead = 0;

    ExtractStatus status = ExtractStatus::Ok;
    BrotliDecoderResult res = BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT;

    for (;;) {
        if (cb.cancel && cb.cancel->IsCancelled()) {
            status = ExtractStatus::Cancelled;
            err = L"cancelled";
            break;
        }

        if (res == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
            DWORD got = 0;
            if (!ReadFile(hIn, inBuf.data(), static_cast<DWORD>(kInBuf), &got,
                          nullptr)) {
                status = ExtractStatus::CorruptInput;
                err = L"read error on brotli input";
                break;
            }
            if (got == 0) {
                // EOF while the decoder still wants input -> truncated stream.
                status = ExtractStatus::CorruptInput;
                err = L"unexpected end of brotli stream";
                break;
            }
            availIn = got;
            nextIn = inBuf.data();
            totalRead += got;
            progress.Emit(totalRead, srcSize, label);
        }

        unsigned char* nextOut = outBuf.data();
        std::size_t availOut = kOutBuf;
        res = BrotliDecoderDecompressStream(state, &availIn, &nextIn, &availOut,
                                            &nextOut, nullptr);

        const std::size_t produced = kOutBuf - availOut;
        if (produced > 0 &&
            !WriteAll(hOut, outBuf.data(), produced)) {
            status = ExtractStatus::WriteFailed;
            err = L"write failed decoding brotli stream";
            break;
        }

        if (res == BROTLI_DECODER_RESULT_SUCCESS) {
            break;  // done; any trailing bytes after the stream are ignored
        }
        if (res == BROTLI_DECODER_RESULT_ERROR) {
            status = ExtractStatus::CorruptInput;
            err = L"corrupt brotli stream";
            break;
        }
        // NEEDS_MORE_INPUT -> read more at the top; NEEDS_MORE_OUTPUT -> loop
        // again with a fresh output buffer and the remaining input.
    }

    BrotliDecoderDestroyInstance(state);
    CloseHandle(hIn);
    return status;
}

// --- Single-file `.br`: decode straight to one output file ------------------
ExtractResult ExtractSingleBr(const PipelinePlan& plan, const std::wstring& src,
                              std::wstring_view stagingDir,
                              const ExtractCallbacks& callbacks) {
    // Output name = inner name hint -> stem -> "output" (matches the libarchive
    // single-stream naming). Strip any path parts defensively.
    std::wstring name = !plan.outputNameHint.empty() ? plan.outputNameHint
                        : !plan.stem.empty()         ? plan.stem
                                                     : std::wstring(L"output");
    name = LeafOnly(name);
    if (name.empty()) name = L"output";

    const std::wstring stagingRoot = NormalizeSeparators(stagingDir);
    const std::wstring outPath = JoinPath(stagingRoot, name);
    const std::wstring outExt = ToExtendedPath(outPath);
    const std::wstring srcExt = ToExtendedPath(src);
    const std::uint64_t srcSize = FileSizeOf(srcExt);

    if (callbacks.cancel && callbacks.cancel->IsCancelled()) {
        return FailBr(ExtractStatus::Cancelled, L"cancelled");
    }

    HANDLE hOut = CreateFileW(outExt.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE) {
        return FailBr(ExtractStatus::WriteFailed,
                      L"cannot create output file: " + name);
    }

    ProgressThrottle progress(callbacks);
    std::wstring err;
    const ExtractStatus st =
        DecodeBrotli(srcExt, hOut, srcSize, callbacks, progress, name, err);
    CloseHandle(hOut);

    if (st != ExtractStatus::Ok) {
        DeleteFileW(outExt.c_str());  // never leave a partial file behind
        return FailBr(st, err);
    }

    progress.Emit(srcSize, srcSize, L"", /*force=*/true);
    ExtractResult r;
    r.status = ExtractStatus::Ok;
    r.entriesWritten = 1;
    return r;
}

// --- `.tar.br`: decode to a temp tar, then untar via libarchive -------------
ExtractResult ExtractTarBr(const PipelinePlan& plan, const std::wstring& src,
                           std::wstring_view stagingDir,
                           const ExtractCallbacks& callbacks) {
    // Decode the brotli wrapper into a temp `.tar` in a sibling hidden dir on the
    // SAME volume as staging, so it never lands among the placed output and is
    // removed on every path.
    const fs::path parent = fs::path(std::wstring(stagingDir)).parent_path();
    const std::wstring tempDir =
        CreateStagingDir(parent.empty() ? std::wstring(stagingDir)
                                        : parent.wstring());
    if (tempDir.empty()) {
        return FailBr(ExtractStatus::WriteFailed,
                      L"cannot create temp dir for .tar.br");
    }
    struct TempCleanup {
        std::wstring dir;
        ~TempCleanup() {
            if (!dir.empty()) RemoveDirTree(dir);
        }
    } cleanup{tempDir};

    const std::wstring tarPath = JoinPath(tempDir, L"inner.tar");
    const std::wstring tarExt = ToExtendedPath(tarPath);
    const std::wstring srcExt = ToExtendedPath(src);
    const std::uint64_t srcSize = FileSizeOf(srcExt);

    if (callbacks.cancel && callbacks.cancel->IsCancelled()) {
        return FailBr(ExtractStatus::Cancelled, L"cancelled");
    }

    HANDLE hOut = CreateFileW(tarExt.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE) {
        return FailBr(ExtractStatus::WriteFailed,
                      L"cannot create temp .tar");
    }

    ProgressThrottle progress(callbacks);
    std::wstring err;
    const ExtractStatus st =
        DecodeBrotli(srcExt, hOut, srcSize, callbacks, progress, L"", err);
    CloseHandle(hOut);
    if (st != ExtractStatus::Ok) {
        return FailBr(st, err);
    }

    // Untar the decoded tar with libarchive, applying the layout to the tar's
    // entries. A synthetic plain-tar plan (no filters) routes to libarchive.
    PipelinePlan sub;
    sub.format = Format::Tar;
    sub.backend = Backend::LibArchive;
    sub.kind = Kind::Container;
    sub.stem = plan.stem;

    LibarchiveExtractor lib;
    return lib.extractToStaging(sub, tarPath, stagingDir, callbacks);
}

}  // namespace

ExtractResult BrotliExtractor::extractToStaging(
    const PipelinePlan& plan, std::wstring_view sourcePath,
    std::wstring_view stagingDir, const ExtractCallbacks& callbacks) {
    if (plan.backend != Backend::Brotli) {
        return FailBr(ExtractStatus::Unsupported,
                      L"plan backend is not brotli");
    }

    // brotli is never multi-volume; the source is the input itself.
    const std::wstring src = plan.firstPartPath.empty()
                                 ? std::wstring(sourcePath)
                                 : plan.firstPartPath;

    // Container kind means a `.tar.br` (brotli wrapping a tar); otherwise it is
    // a single-file `.br`. The shape is decided by the detector (extension only)
    // — a `.br` is NEVER untarred by content sniffing.
    if (plan.kind == Kind::Container) {
        return ExtractTarBr(plan, src, stagingDir, callbacks);
    }
    return ExtractSingleBr(plan, src, stagingDir, callbacks);
}

}  // namespace ae
