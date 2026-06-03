// Tests for the bit7z / 7z.dll backend (task 09): plain 7z, encrypted 7z
// (regular + header-encrypted), 7z multi-volume split (incl. launching on a
// non-first part and a missing-volume error), the RAR family (gated/skipped
// when the corpus has no rar.exe-generated assets), the lazy 7z.dll load
// guarantee, and progress/cancel parity with the libarchive backend.
//
// Every test that extracts uses a UNIQUE temp dir and deletes it on every path
// (ScopedTempDir RAII), never touching the source tree or tests/assets.

#include "archive_core/extract.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "archive_core/detect.h"

namespace fs = std::filesystem;

using ae::Backend;
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
using ae::ResolveBackend;
using ae::SevenZipExtractor;
using ae::ToExtendedPath;
using ae::VerifyMultiVolumeComplete;

namespace {

#ifndef AE_TEST_ASSETS_DIR
#error "AE_TEST_ASSETS_DIR must be defined (see CMakeLists.txt)."
#endif

fs::path AssetPath(const char* name) {
    return fs::path(AE_TEST_ASSETS_DIR) / name;
}

bool AssetExists(const char* name) {
    std::error_code ec;
    return fs::exists(AssetPath(name), ec);
}

// Unique hidden temp dir under %TEMP%, removed on destruction (RAII). Reuses
// the engine's own helpers so extended-path/hidden children are handled.
class ScopedTempDir {
   public:
    ScopedTempDir() {
        wchar_t tmp[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, tmp);
        std::wstring base(tmp, n);
        path_ = CreateStagingDir(base);
        if (path_.empty()) {
            path_ = base + L"ae-7z-test-fallback";
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

// Copy a single asset into `destDir`, returning the destination path.
std::wstring CopyAssetTo(const char* asset, const std::wstring& destDir,
                         const wchar_t* destName) {
    const fs::path src = AssetPath(asset);
    const fs::path dst = fs::path(destDir) / destName;
    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    return ec ? std::wstring() : dst.wstring();
}

// Run SevenZipExtractor on a plan + source with an optional password callback.
ExtractResult Extract7z(const PipelinePlan& plan, const std::wstring& src,
                        const std::wstring& staging,
                        const wchar_t* password = nullptr) {
    SevenZipExtractor ex;
    ExtractCallbacks cb;
    std::wstring pw = password ? password : L"";
    if (password) {
        cb.requestPassword =
            [pw](const ae::PasswordPrompt&) -> std::optional<std::wstring> {
            return pw;
        };
    }
    return ex.extractToStaging(plan, src, staging, cb);
}

constexpr const wchar_t* kPassword = L"test1234";

}  // namespace

// --- Routing: encryption query completes the re-route ----------------------

TEST(SevenZip, PlainSevenZipStaysOnLibarchive) {
    const std::wstring src = AssetPath("7z__multi-files.7z").wstring();
    PipelinePlan plan = DetectFile(src);
    EXPECT_EQ(plan.backend, Backend::LibArchive);  // detector's tentative route
    EXPECT_EQ(ResolveBackend(plan, src), Backend::LibArchive);  // not encrypted
}

TEST(SevenZip, EncryptedSevenZipReroutesToSevenZipDll) {
    const std::wstring src = AssetPath("7z-aes__password.7z").wstring();
    PipelinePlan plan = DetectFile(src);
    // Data-encrypted (header readable): detector tentatively routes to
    // libarchive; the open-time query must re-route to the 7z.dll backend.
    EXPECT_EQ(ResolveBackend(plan, src), Backend::SevenZipDll);
}

TEST(SevenZip, HeaderEncryptedSevenZipReroutesToSevenZipDll) {
    const std::wstring src = AssetPath("7z-aes-header__password.7z").wstring();
    PipelinePlan plan = DetectFile(src);
    EXPECT_EQ(ResolveBackend(plan, src), Backend::SevenZipDll);
}

// --- Plain 7z via the bit7z backend (parity with libarchive) ---------------

TEST(SevenZip, PlainSevenZipExtractsViaBit7z) {
    ScopedTempDir tmp;
    const std::wstring src = AssetPath("7z__multi-files.7z").wstring();
    PipelinePlan plan = DetectFile(src);
    plan.backend = Backend::SevenZipDll;  // force the 7z.dll path explicitly
    const auto r = Extract7z(plan, src, tmp.path());
    EXPECT_EQ(r.status, ExtractStatus::Ok);
    EXPECT_GE(CountFiles(tmp.path()), 3u);
}

// --- Encrypted 7z: NeedPassword without, success with test1234 -------------

TEST(SevenZip, EncryptedSevenZipNeedsPasswordWhenNoneSupplied) {
    ScopedTempDir tmp;
    const std::wstring src = AssetPath("7z-aes__password.7z").wstring();
    PipelinePlan plan = DetectFile(src);
    plan.backend = Backend::SevenZipDll;
    const auto r = Extract7z(plan, src, tmp.path(), /*password=*/nullptr);
    EXPECT_EQ(r.status, ExtractStatus::NeedPassword);
    EXPECT_EQ(CountFiles(tmp.path()), 0u);  // nothing decrypted
}

TEST(SevenZip, EncryptedSevenZipExtractsWithPassword) {
    ScopedTempDir tmp;
    const std::wstring src = AssetPath("7z-aes__password.7z").wstring();
    PipelinePlan plan = DetectFile(src);
    plan.backend = Backend::SevenZipDll;
    const auto r = Extract7z(plan, src, tmp.path(), kPassword);
    EXPECT_EQ(r.status, ExtractStatus::Ok);
    EXPECT_GE(CountFiles(tmp.path()), 1u);
}

TEST(SevenZip, HeaderEncryptedSevenZipExtractsWithPassword) {
    ScopedTempDir tmp;
    const std::wstring src = AssetPath("7z-aes-header__password.7z").wstring();
    PipelinePlan plan = DetectFile(src);
    plan.backend = Backend::SevenZipDll;
    const auto r = Extract7z(plan, src, tmp.path(), kPassword);
    EXPECT_EQ(r.status, ExtractStatus::Ok);
    EXPECT_GE(CountFiles(tmp.path()), 1u);
}

TEST(SevenZip, HeaderEncryptedSevenZipNeedsPasswordWhenNoneSupplied) {
    ScopedTempDir tmp;
    const std::wstring src = AssetPath("7z-aes-header__password.7z").wstring();
    PipelinePlan plan = DetectFile(src);
    plan.backend = Backend::SevenZipDll;
    const auto r = Extract7z(plan, src, tmp.path(), /*password=*/nullptr);
    EXPECT_EQ(r.status, ExtractStatus::NeedPassword);
    EXPECT_EQ(CountFiles(tmp.path()), 0u);
}

// --- Multi-volume: extract from the first part ------------------------------

TEST(SevenZip, MultiVolumeExtractsFromFirstPart) {
    ScopedTempDir tmp;
    const std::wstring src = AssetPath("edge-multivolume.7z.001").wstring();
    PipelinePlan plan = DetectFile(src);
    EXPECT_EQ(plan.volumeFamily, ae::VolumeFamily::SevenZip);
    plan.backend = Backend::SevenZipDll;
    const auto r = Extract7z(plan, src, tmp.path());
    EXPECT_EQ(r.status, ExtractStatus::Ok) << "msg=" << r.message.c_str();
    EXPECT_GE(CountFiles(tmp.path()), 1u);
}

// --- Multi-volume: launch on a NON-FIRST part, still extract ----------------

TEST(SevenZip, MultiVolumeExtractsWhenLaunchedOnNonFirstPart) {
    ScopedTempDir tmp;
    // The detector resolves firstPartPath from a .003 input; bit7z opens .001.
    const std::wstring src = AssetPath("edge-multivolume.7z.003").wstring();
    PipelinePlan plan = DetectFile(src);
    ASSERT_EQ(plan.volumeFamily, ae::VolumeFamily::SevenZip);
    ASSERT_FALSE(plan.firstPartPath.empty());
    plan.backend = Backend::SevenZipDll;
    const auto r = Extract7z(plan, plan.firstPartPath, tmp.path());
    EXPECT_EQ(r.status, ExtractStatus::Ok) << "msg=" << r.message.c_str();
    EXPECT_GE(CountFiles(tmp.path()), 1u);
}

// --- Multi-volume: a MISSING part yields a named-volume error + clean temp --

TEST(SevenZip, MultiVolumeMissingPartNamedError) {
    ScopedTempDir volsHolder;  // holds the copied parts (separate from output)
    ScopedTempDir outDir;

    // Copy all five parts into a temp dir, then delete part .003.
    const char* parts[] = {"edge-multivolume.7z.001", "edge-multivolume.7z.002",
                           "edge-multivolume.7z.003", "edge-multivolume.7z.004",
                           "edge-multivolume.7z.005"};
    std::wstring first;
    for (const char* p : parts) {
        const std::wstring name = fs::path(p).filename().wstring();
        const std::wstring dst = CopyAssetTo(p, volsHolder.path(), name.c_str());
        ASSERT_FALSE(dst.empty());
        if (first.empty()) first = dst;
    }
    // Delete the middle volume.
    const std::wstring gone =
        (fs::path(volsHolder.path()) / L"edge-multivolume.7z.003").wstring();
    ASSERT_NE(DeleteFileW(ToExtendedPath(gone).c_str()), 0);

    PipelinePlan plan = DetectFile(first);
    ASSERT_EQ(plan.volumeFamily, ae::VolumeFamily::SevenZip);
    plan.backend = Backend::SevenZipDll;

    // The completeness check must fail and name the missing volume.
    std::wstring missing;
    EXPECT_FALSE(VerifyMultiVolumeComplete(plan, first, missing));
    EXPECT_EQ(missing, L"edge-multivolume.7z.003");

    const auto r = Extract7z(plan, first, outDir.path());
    EXPECT_EQ(r.status, ExtractStatus::CorruptInput);
    EXPECT_NE(r.message.find(L"edge-multivolume.7z.003"), std::wstring::npos);
    // No truncated output left behind.
    EXPECT_EQ(CountFiles(outDir.path()), 0u);
}

// --- Lazy 7z.dll load: NOT loaded for a plain zip (libarchive path) ---------

TEST(SevenZip, DllNotLoadedForPlainZip) {
    // Guard: this assertion is only meaningful if 7z.dll hasn't been loaded by
    // a prior test in this process. Skip if it's already resident.
    if (GetModuleHandleW(L"7z.dll") != nullptr) {
        GTEST_SKIP() << "7z.dll already loaded by an earlier 7z test in-process";
    }
    ScopedTempDir tmp;
    const std::wstring src = AssetPath("zip__multi-files.zip").wstring();
    PipelinePlan plan = DetectFile(src);
    ASSERT_EQ(plan.backend, Backend::LibArchive);
    LibarchiveExtractor ex;
    ExtractCallbacks cb;
    const auto r = ex.extractToStaging(plan, src, tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Ok);
    // The libarchive path must never have triggered the on-demand DLL load.
    EXPECT_EQ(GetModuleHandleW(L"7z.dll"), nullptr);
}

// --- Progress + cancel parity with the libarchive backend -------------------

TEST(SevenZip, ProgressIsReported) {
    ScopedTempDir tmp;
    const std::wstring src = AssetPath("7z__multi-files.7z").wstring();
    PipelinePlan plan = DetectFile(src);
    plan.backend = Backend::SevenZipDll;
    SevenZipExtractor ex;
    ExtractCallbacks cb;
    std::atomic<int> calls{0};
    cb.onProgress = [&](const Progress&) { ++calls; };
    const auto r = ex.extractToStaging(plan, src, tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Ok);
    EXPECT_GE(calls.load(), 1);  // at least the final forced beat
}

TEST(SevenZip, CancelBeforeStartStopsCleanly) {
    ScopedTempDir tmp;
    const std::wstring src = AssetPath("7z__multi-files.7z").wstring();
    PipelinePlan plan = DetectFile(src);
    plan.backend = Backend::SevenZipDll;
    CancellationToken token;
    token.Cancel();
    SevenZipExtractor ex;
    ExtractCallbacks cb;
    cb.cancel = &token;
    const auto r = ex.extractToStaging(plan, src, tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Cancelled);
    EXPECT_EQ(CountFiles(tmp.path()), 0u);
}

TEST(SevenZip, CancelMidStreamFromProgressCallback) {
    ScopedTempDir tmp;
    const std::wstring src = AssetPath("7z__multi-files.7z").wstring();
    PipelinePlan plan = DetectFile(src);
    plan.backend = Backend::SevenZipDll;
    CancellationToken token;
    SevenZipExtractor ex;
    ExtractCallbacks cb;
    cb.cancel = &token;
    cb.onProgress = [&](const Progress&) { token.Cancel(); };
    const auto r = ex.extractToStaging(plan, src, tmp.path(), cb);
    // Either it finished a tiny archive before the first tick (Ok) or aborted.
    EXPECT_TRUE(r.status == ExtractStatus::Cancelled ||
                r.status == ExtractStatus::Ok)
        << "status=" << static_cast<int>(r.status);
}

// --- RAR family: GATED on asset presence (absent in this corpus -> SKIP) ----

TEST(SevenZip, RarPlainExtractsViaBit7z) {
    if (!AssetExists("rar__multi-files.rar")) {
        GTEST_SKIP() << "rar assets absent (no rar.exe in corpus generator)";
    }
    ScopedTempDir tmp;
    const std::wstring src = AssetPath("rar__multi-files.rar").wstring();
    PipelinePlan plan = DetectFile(src);
    EXPECT_EQ(plan.backend, Backend::SevenZipDll);  // RAR always -> 7z.dll
    const auto r = Extract7z(plan, src, tmp.path());
    EXPECT_EQ(r.status, ExtractStatus::Ok) << "msg=" << r.message.c_str();
    EXPECT_GE(CountFiles(tmp.path()), 1u);
}

TEST(SevenZip, RarSingleFolderPreservesTree) {
    if (!AssetExists("rar__single-folder.rar")) {
        GTEST_SKIP() << "rar assets absent (no rar.exe in corpus generator)";
    }
    ScopedTempDir tmp;
    const std::wstring src = AssetPath("rar__single-folder.rar").wstring();
    PipelinePlan plan = DetectFile(src);
    EXPECT_EQ(plan.backend, Backend::SevenZipDll);
    const auto r = Extract7z(plan, src, tmp.path());
    EXPECT_EQ(r.status, ExtractStatus::Ok) << "msg=" << r.message.c_str();
}

TEST(SevenZip, RarEncryptedExtractsWithPassword) {
    if (!AssetExists("rar-aes__password.rar")) {
        GTEST_SKIP() << "rar-aes assets absent (no rar.exe in corpus generator)";
    }
    ScopedTempDir tmp;
    const std::wstring src = AssetPath("rar-aes__password.rar").wstring();
    PipelinePlan plan = DetectFile(src);
    EXPECT_EQ(plan.backend, Backend::SevenZipDll);
    const auto r = Extract7z(plan, src, tmp.path(), kPassword);
    EXPECT_EQ(r.status, ExtractStatus::Ok) << "msg=" << r.message.c_str();
}

// RAR routing is verifiable WITHOUT the asset: the detector routes any .rar to
// the 7z.dll backend purely from the extension, so this stays green always.
TEST(SevenZip, RarRoutingIsAlwaysSevenZipDll) {
    PipelinePlan plan = DetectFile(L"C:/does-not-exist/sample.rar");
    EXPECT_EQ(plan.backend, Backend::SevenZipDll);
    EXPECT_EQ(ResolveBackend(plan, L"C:/does-not-exist/sample.rar"),
              Backend::SevenZipDll);
}
