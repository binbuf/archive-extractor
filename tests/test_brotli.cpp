// Task 11 — brotli (.br) backend + compound-format finalization.
//
// Covers: single-file `.br` name-strip (page.html.br -> page.html) and the
// stemless fallback (a file named ".br"); `.tar.br` decode+untar with layout;
// and a cross-filter compound matrix confirming that a compressor wrapping a tar
// is untarred while one wrapping a non-tar yields a single file — decided by the
// inner content after the filter, uniformly for gz/bz2/xz/zst/lz4/br.
//
// Every test extracts into a unique temp dir and deletes it immediately on every
// path (ScopedTempDir RAII), per the testing conventions.

#include "archive_core/extract.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "archive_core/detect.h"

namespace fs = std::filesystem;

using ae::Backend;
using ae::BrotliExtractor;
using ae::CreateStagingDir;
using ae::DetectFile;
using ae::ExtractCallbacks;
using ae::ExtractResult;
using ae::ExtractStatus;
using ae::IExtractor;
using ae::Kind;
using ae::LibarchiveExtractor;
using ae::PipelinePlan;
using ae::RemoveDirTree;

namespace {

#ifndef AE_TEST_ASSETS_DIR
#error "AE_TEST_ASSETS_DIR must be defined (see CMakeLists.txt)."
#endif

fs::path AssetPath(const wchar_t* name) {
    return fs::path(AE_TEST_ASSETS_DIR) / name;
}

bool AssetExists(const wchar_t* name) {
    std::error_code ec;
    return fs::exists(AssetPath(name), ec);
}

// Unique temp dir under %TEMP%, removed on destruction on every path.
class ScopedTempDir {
   public:
    ScopedTempDir() {
        wchar_t tmp[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, tmp);
        std::wstring base(tmp, n);
        path_ = CreateStagingDir(base);
        if (path_.empty()) {
            path_ = base + L"ae-brotli-test-fallback";
            CreateDirectoryW(path_.c_str(), nullptr);
        }
    }
    ~ScopedTempDir() {
        if (!path_.empty()) RemoveDirTree(path_);
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;
    const std::wstring& path() const { return path_; }

   private:
    std::wstring path_;
};

bool Exists(const std::wstring& dir, const wchar_t* rel) {
    std::error_code ec;
    return fs::exists(fs::path(dir) / rel, ec);
}

std::size_t CountFiles(const std::wstring& dir) {
    std::error_code ec;
    std::size_t n = 0;
    for (auto it = fs::recursive_directory_iterator(fs::path(dir), ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) ++n;
    }
    return n;
}

std::string ReadFileBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// Extract an asset by routing to the backend the detector + ResolveBackend pick
// (mirrors the app's dispatch). Only libarchive/brotli are needed for this
// suite's matrix.
ExtractResult Extract(const wchar_t* asset, const std::wstring& staging) {
    const std::wstring src = AssetPath(asset).wstring();
    PipelinePlan plan = DetectFile(src);
    plan.backend = ae::ResolveBackend(plan, src);

    LibarchiveExtractor lib;
    BrotliExtractor br;
    ExtractCallbacks cb;
    IExtractor& ex = (plan.backend == Backend::Brotli)
                         ? static_cast<IExtractor&>(br)
                         : static_cast<IExtractor&>(lib);
    return ex.extractToStaging(plan, src, staging, cb);
}

}  // namespace

// --- Single-file `.br`: name-strip ------------------------------------------

TEST(Brotli, SingleFileStripsBrExtension) {
    const PipelinePlan plan = DetectFile(AssetPath(L"page.html.br").wstring());
    EXPECT_EQ(plan.backend, Backend::Brotli);
    EXPECT_EQ(plan.kind, Kind::SingleStream);

    ScopedTempDir tmp;
    const auto r = Extract(L"page.html.br", tmp.path());
    ASSERT_EQ(r.status, ExtractStatus::Ok);
    EXPECT_EQ(CountFiles(tmp.path()), 1u);
    EXPECT_TRUE(Exists(tmp.path(), L"page.html"));
    // Content round-trips to the generator's known HTML payload.
    const std::string got = ReadFileBytes(fs::path(tmp.path()) / L"page.html");
    EXPECT_NE(got.find("<title>brotli</title>"), std::string::npos);
}

// --- Single-file `.br`: stemless fallback -----------------------------------

TEST(Brotli, StemlessFallsBackToStem) {
    // A file literally named ".br": stripping ".br" leaves nothing, so the
    // output name falls back (stem -> "output"). It must still yield exactly one
    // file and never an empty name or an escape from staging.
    if (!AssetExists(L".br")) {
        GTEST_SKIP() << "stemless .br asset not generated";
    }
    const PipelinePlan plan = DetectFile(AssetPath(L".br").wstring());
    EXPECT_EQ(plan.backend, Backend::Brotli);
    EXPECT_EQ(plan.kind, Kind::SingleStream);
    EXPECT_TRUE(plan.outputNameHint.empty());  // nothing remained after strip

    ScopedTempDir tmp;
    const auto r = Extract(L".br", tmp.path());
    ASSERT_EQ(r.status, ExtractStatus::Ok);
    EXPECT_EQ(CountFiles(tmp.path()), 1u);
    EXPECT_TRUE(Exists(tmp.path(), L"output"));
}

// --- `.tar.br`: decode then untar with layout -------------------------------

TEST(Brotli, TarBrDecodesThenUntars) {
    const PipelinePlan plan =
        DetectFile(AssetPath(L"tarbr__single-folder.tar.br").wstring());
    EXPECT_EQ(plan.backend, Backend::Brotli);
    EXPECT_EQ(plan.kind, Kind::Container);

    ScopedTempDir tmp;
    const auto r = Extract(L"tarbr__single-folder.tar.br", tmp.path());
    ASSERT_EQ(r.status, ExtractStatus::Ok);
    // The inner tar is fully expanded — no intermediate .tar left in the output.
    EXPECT_TRUE(Exists(tmp.path(), L"project\\README.md"));
    EXPECT_TRUE(Exists(tmp.path(), L"project\\main.py"));
    EXPECT_FALSE(Exists(tmp.path(), L"inner.tar"));
    EXPECT_FALSE(Exists(tmp.path(), L"project.tar"));
}

TEST(Brotli, TarBrMultiFilesUntarsAllEntries) {
    ScopedTempDir tmp;
    const auto r = Extract(L"tarbr__multi-files.tar.br", tmp.path());
    ASSERT_EQ(r.status, ExtractStatus::Ok);
    EXPECT_GE(CountFiles(tmp.path()), 3u);
}

// --- Cross-filter compound matrix -------------------------------------------
// For every single-stream filter: the tarred payload is untarred (2+ files from
// a folder tree) while the bare payload yields exactly one file. The untar vs
// one-file decision is made by the inner content after the filter, uniformly.

TEST(Brotli, CompoundMatrixTarredVsBareForEveryFilter) {
    struct Case {
        const wchar_t* tarred;  // wraps a tar -> untarred
        const wchar_t* bare;    // wraps a non-tar -> one file
        const wchar_t* bareInner;
    };
    const Case cases[] = {
        {L"targz__single-folder.tar.gz", L"document.txt.gz", L"document.txt"},
        {L"tarbz2__single-folder.tar.bz2", L"notes.txt.bz2", L"notes.txt"},
        {L"tarxz__single-folder.tar.xz", L"data.csv.xz", L"data.csv"},
        {L"tarzst__single-folder.tar.zst", L"payload.json.zst", L"payload.json"},
        {L"tarlz4__single-folder.tar.lz4", L"frame.bin.lz4", L"frame.bin"},
        {L"tarbr__single-folder.tar.br", L"page.html.br", L"page.html"},
    };

    for (const auto& c : cases) {
        // Tarred -> untarred: the folder tree is expanded (multiple files).
        {
            ScopedTempDir tmp;
            const auto r = Extract(c.tarred, tmp.path());
            ASSERT_EQ(r.status, ExtractStatus::Ok) << c.tarred;
            EXPECT_GE(CountFiles(tmp.path()), 2u) << c.tarred;
            EXPECT_TRUE(Exists(tmp.path(), L"project\\README.md")) << c.tarred;
        }
        // Bare -> exactly one file with the inner name.
        {
            ScopedTempDir tmp;
            const auto r = Extract(c.bare, tmp.path());
            ASSERT_EQ(r.status, ExtractStatus::Ok) << c.bare;
            EXPECT_EQ(CountFiles(tmp.path()), 1u) << c.bare;
            EXPECT_TRUE(Exists(tmp.path(), c.bareInner)) << c.bare;
        }
    }
}
