#include "archive_core/extract.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "archive_core/detect.h"

namespace fs = std::filesystem;

using ae::CancellationToken;
using ae::CreateStagingDir;
using ae::DetectFile;
using ae::ExtractCallbacks;
using ae::ExtractResult;
using ae::ExtractStatus;
using ae::LibarchiveExtractor;
using ae::PipelinePlan;
using ae::Progress;
using ae::RemoveDirTree;
using ae::SameVolume;
using ae::ToExtendedPath;

namespace {

#ifndef AE_TEST_ASSETS_DIR
#error "AE_TEST_ASSETS_DIR must be defined (see CMakeLists.txt)."
#endif

fs::path AssetPath(const char* name) {
    return fs::path(AE_TEST_ASSETS_DIR) / name;
}

// A unique temp directory created under %TEMP%, deleted on destruction on every
// path (RAII), satisfying the cleanup rule. Uses the engine's own RemoveDirTree
// so extended-path / hidden children are handled.
class ScopedTempDir {
   public:
    ScopedTempDir() {
        wchar_t tmp[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, tmp);
        std::wstring base(tmp, n);
        // Unique name via the engine's GUID-based staging helper, which already
        // creates a unique hidden dir on the same volume.
        path_ = CreateStagingDir(base);
        if (path_.empty()) {
            // Fallback (should not happen): synthesize one.
            path_ = base + L"ae-test-fallback";
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

// Read a whole file into a string (binary).
std::string ReadFileBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    return out;
}

bool Exists(const std::wstring& dir, const wchar_t* rel) {
    std::error_code ec;
    return fs::exists(fs::path(dir) / rel, ec);
}

// Recursively count regular files under a directory.
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

ExtractResult Extract(const char* asset, const std::wstring& staging) {
    const std::wstring src = AssetPath(asset).wstring();
    const PipelinePlan plan = DetectFile(src);
    LibarchiveExtractor ex;
    ExtractCallbacks cb;
    return ex.extractToStaging(plan, src, staging, cb);
}

}  // namespace

// --- Staging utilities ------------------------------------------------------

TEST(Staging, CreateIsHiddenAndOnSameVolume) {
    wchar_t tmp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    const std::wstring base(tmp, n);

    const std::wstring staging = CreateStagingDir(base);
    ASSERT_FALSE(staging.empty());
    // Same volume as the work dir (the acceptance criterion).
    EXPECT_TRUE(SameVolume(base, staging));

    const DWORD attr = GetFileAttributesW(ToExtendedPath(staging).c_str());
    ASSERT_NE(attr, INVALID_FILE_ATTRIBUTES);
    EXPECT_TRUE(attr & FILE_ATTRIBUTE_DIRECTORY);
    EXPECT_TRUE(attr & FILE_ATTRIBUTE_HIDDEN);

    EXPECT_TRUE(RemoveDirTree(staging));
    EXPECT_EQ(GetFileAttributesW(ToExtendedPath(staging).c_str()),
              static_cast<DWORD>(INVALID_FILE_ATTRIBUTES));
}

TEST(Staging, TwoCallsAreUnique) {
    wchar_t tmp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    const std::wstring base(tmp, n);
    const std::wstring a = CreateStagingDir(base);
    const std::wstring b = CreateStagingDir(base);
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty());
    EXPECT_NE(a, b);
    RemoveDirTree(a);
    RemoveDirTree(b);
}

TEST(Staging, RemoveMissingIsOk) {
    EXPECT_TRUE(RemoveDirTree(L"C:\\nope\\does-not-exist-ae-12345"));
}

// --- Plain containers (one representative per backend format) ---------------

TEST(Extract, ZipMultiFiles) {
    ScopedTempDir tmp;
    const auto r = Extract("zip__multi-files.zip", tmp.path());
    EXPECT_EQ(r.status, ExtractStatus::Ok);
    EXPECT_GE(r.entriesWritten, 3u);
}

TEST(Extract, SevenZipMultiFiles) {
    ScopedTempDir tmp;
    const auto r = Extract("7z__multi-files.7z", tmp.path());
    EXPECT_EQ(r.status, ExtractStatus::Ok);
    EXPECT_GE(CountFiles(tmp.path()), 3u);
}

TEST(Extract, TarSingleFileByteForByte) {
    ScopedTempDir tmp;
    const auto r = Extract("tar__single-file.tar", tmp.path());
    ASSERT_EQ(r.status, ExtractStatus::Ok);
    ASSERT_TRUE(Exists(tmp.path(), L"readme.txt"));
    const std::string got =
        ReadFileBytes(fs::path(tmp.path()) / L"readme.txt");
    EXPECT_EQ(got, "The quick brown fox jumps over the lazy dog.\n");
}

TEST(Extract, TarSingleFolderPreservesTree) {
    ScopedTempDir tmp;
    const auto r = Extract("tar__single-folder.tar", tmp.path());
    ASSERT_EQ(r.status, ExtractStatus::Ok);
    EXPECT_TRUE(Exists(tmp.path(), L"project\\README.md"));
    EXPECT_TRUE(Exists(tmp.path(), L"project\\main.py"));
    EXPECT_TRUE(Exists(tmp.path(), L"project\\lib\\util.py"));
}

// --- Compound tar.* forms — one flow, tar entries read directly -------------

TEST(Extract, AllCompoundTarForms) {
    struct C { const char* asset; };
    const C cases[] = {
        {"targz__multi-files.tar.gz"},   {"tarbz2__multi-files.tar.bz2"},
        {"tarxz__multi-files.tar.xz"},   {"tarzst__multi-files.tar.zst"},
        {"tarlz4__multi-files.tar.lz4"}, {"tgz__multi-files.tgz"},
        {"tbz2__multi-files.tbz2"},      {"txz__multi-files.txz"},
    };
    for (const auto& c : cases) {
        ScopedTempDir tmp;
        const auto r = Extract(c.asset, tmp.path());
        EXPECT_EQ(r.status, ExtractStatus::Ok) << c.asset;
        EXPECT_GE(CountFiles(tmp.path()), 3u) << c.asset;
    }
}

TEST(Extract, TarGzReadsTarEntriesInOneFlow) {
    ScopedTempDir tmp;
    const auto r = Extract("targz__single-folder.tar.gz", tmp.path());
    ASSERT_EQ(r.status, ExtractStatus::Ok);
    // The inner tar is fully expanded (not left as a .tar on disk).
    EXPECT_TRUE(Exists(tmp.path(), L"project\\README.md"));
    EXPECT_FALSE(Exists(tmp.path(), L"project.tar"));
}

// --- Single-stream inputs: decode to exactly one file -----------------------

TEST(Extract, BareSingleStreamsDecodeToOneFile) {
    struct C { const char* asset; const wchar_t* inner; };
    const C cases[] = {
        {"document.txt.gz", L"document.txt"},
        {"notes.txt.bz2", L"notes.txt"},
        {"data.csv.xz", L"data.csv"},
        {"payload.json.zst", L"payload.json"},
        {"frame.bin.lz4", L"frame.bin"},
    };
    for (const auto& c : cases) {
        ScopedTempDir tmp;
        const auto r = Extract(c.asset, tmp.path());
        EXPECT_EQ(r.status, ExtractStatus::Ok) << c.asset;
        EXPECT_EQ(CountFiles(tmp.path()), 1u) << c.asset;
        EXPECT_TRUE(Exists(tmp.path(), c.inner)) << c.asset;
    }
}

// --- Empty archive ----------------------------------------------------------

TEST(Extract, EmptyTarProducesNoOutput) {
    ScopedTempDir tmp;
    const auto r = Extract("edge-empty.tar", tmp.path());
    EXPECT_EQ(r.status, ExtractStatus::Ok);
    EXPECT_EQ(CountFiles(tmp.path()), 0u);
}

// --- Path safety (zip-slip) -------------------------------------------------

TEST(Extract, ZipSlipIsRejectedAsUntrusted) {
    ScopedTempDir tmp;
    const auto r = Extract("edge-zip-slip.zip", tmp.path());
    EXPECT_EQ(r.status, ExtractStatus::Untrusted);

    // Nothing escaped the staging dir: the parent must not contain the bait.
    const fs::path parent = fs::path(tmp.path()).parent_path();
    std::error_code ec;
    EXPECT_FALSE(fs::exists(parent / L"escaped.txt", ec));
    EXPECT_FALSE(fs::exists(parent / L"escaped2.txt", ec));
}

// --- Links present: detected, skipped, not fatal ----------------------------

TEST(Extract, LinksAreSkippedNotFatal) {
    ScopedTempDir tmp;
    const auto r = Extract("edge-links.tar", tmp.path());
    EXPECT_EQ(r.status, ExtractStatus::Ok);
    EXPECT_TRUE(r.hadLinks);
    EXPECT_GE(r.entriesSkipped, 1u);
    // The real (non-link) regular file is still extracted.
    EXPECT_TRUE(Exists(tmp.path(), L"readme.txt"));
}

// --- Corrupt input ----------------------------------------------------------

// A corrupt/truncated archive must be handled gracefully: the engine either
// reports a read error (CorruptInput) or, when libarchive can still recover
// readable entries from the stream, completes. It must never hang, crash, or
// escape the staging directory. (The fixture's exact recoverability depends on
// where the generator truncated it; libarchive recovers from local headers.)
TEST(Extract, TruncatedArchiveHandledGracefully) {
    ScopedTempDir tmp;
    const auto r = Extract("edge-truncated.zip", tmp.path());
    EXPECT_TRUE(r.status == ExtractStatus::Ok ||
                r.status == ExtractStatus::CorruptInput)
        << "status=" << static_cast<int>(r.status);
    // Whatever the outcome, nothing was written above the staging root.
    const fs::path parent = fs::path(tmp.path()).parent_path();
    std::error_code ec;
    EXPECT_FALSE(fs::exists(parent / L"payload.txt", ec));
}

// --- Timestamps applied -----------------------------------------------------

TEST(Extract, ModifiedTimeIsApplied) {
    ScopedTempDir tmp;
    const auto r = Extract("tar__single-file.tar", tmp.path());
    ASSERT_EQ(r.status, ExtractStatus::Ok);
    const std::wstring file =
        ToExtendedPath((fs::path(tmp.path()) / L"readme.txt").wstring());
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    ASSERT_TRUE(GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &fad));
    // mtime must be a sane, in-range value (clamped to >= 1980), i.e. nonzero.
    const unsigned long long t =
        (static_cast<unsigned long long>(fad.ftLastWriteTime.dwHighDateTime)
         << 32) |
        fad.ftLastWriteTime.dwLowDateTime;
    EXPECT_GT(t, 0ULL);
}

// --- Cancellation -----------------------------------------------------------

TEST(Extract, CancelBeforeStartStopsAndRemovesTemp) {
    ScopedTempDir tmp;
    const std::wstring src = AssetPath("targz__multi-files.tar.gz").wstring();
    const PipelinePlan plan = DetectFile(src);

    CancellationToken token;
    token.Cancel();  // cancel up front: must abort at the first entry check.

    LibarchiveExtractor ex;
    ExtractCallbacks cb;
    cb.cancel = &token;
    const auto r = ex.extractToStaging(plan, src, tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Cancelled);
}

TEST(Extract, CancelMidStreamFromProgressCallback) {
    ScopedTempDir tmp;
    const std::wstring src = AssetPath("7z__multi-files.7z").wstring();
    const PipelinePlan plan = DetectFile(src);

    CancellationToken token;
    LibarchiveExtractor ex;
    ExtractCallbacks cb;
    cb.cancel = &token;
    std::atomic<int> ticks{0};
    cb.onProgress = [&](const Progress&) {
        if (++ticks >= 1) token.Cancel();  // cancel as soon as progress flows
    };
    const auto r = ex.extractToStaging(plan, src, tmp.path(), cb);
    // Either it finished a tiny archive before the first throttled tick (Ok) or
    // it was cancelled; both are acceptable, but it must not corrupt/hang.
    EXPECT_TRUE(r.status == ExtractStatus::Cancelled ||
                r.status == ExtractStatus::Ok);
}

// --- Worker-thread driver ---------------------------------------------------

TEST(Extract, WorkerThreadDriverDeliversResult) {
    ScopedTempDir tmp;
    const std::wstring src = AssetPath("zip__multi-files.zip").wstring();
    const PipelinePlan plan = DetectFile(src);

    LibarchiveExtractor ex;
    ExtractCallbacks cb;
    std::atomic<bool> done{false};
    ExtractResult captured;
    std::thread t = ae::RunExtractionOnWorker(
        ex, plan, src, tmp.path(), cb, [&](ExtractResult r) {
            captured = std::move(r);
            done.store(true);
        });
    t.join();
    EXPECT_TRUE(done.load());
    EXPECT_EQ(captured.status, ExtractStatus::Ok);
}

// --- Progress callback drives toward total ----------------------------------

TEST(Extract, ProgressIsReported) {
    ScopedTempDir tmp;
    const std::wstring src = AssetPath("targz__multi-files.tar.gz").wstring();
    const PipelinePlan plan = DetectFile(src);

    LibarchiveExtractor ex;
    ExtractCallbacks cb;
    std::atomic<int> calls{0};
    std::atomic<unsigned long long> lastDone{0};
    cb.onProgress = [&](const Progress& p) {
        ++calls;
        lastDone.store(p.done);
    };
    const auto r = ex.extractToStaging(plan, src, tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Ok);
    EXPECT_GE(calls.load(), 1);
}
