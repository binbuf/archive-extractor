#include "archive_core/layout.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "archive_core/detect.h"
#include "archive_core/extract.h"

namespace fs = std::filesystem;

using ae::CreateStagingDir;
using ae::DetectFile;
using ae::ExtractCallbacks;
using ae::ExtractStatus;
using ae::Kind;
using ae::LayoutPlanner;
using ae::LibarchiveExtractor;
using ae::MakeNonCollidingName;
using ae::PipelinePlan;
using ae::PlaceResult;
using ae::PlaceStatus;
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

// A unique temp directory under %TEMP%, deleted on destruction on EVERY path
// (RAII) via the engine's RemoveDirTree. Same pattern as test_extract.cpp.
class ScopedTempDir {
   public:
    ScopedTempDir() {
        wchar_t tmp[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, tmp);
        std::wstring base(tmp, n);
        path_ = CreateStagingDir(base);
        if (path_.empty()) {
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

bool Exists(const std::wstring& dir, const wchar_t* rel) {
    std::error_code ec;
    return fs::exists(fs::path(dir) / rel, ec);
}

bool IsDir(const std::wstring& p) {
    std::error_code ec;
    return fs::is_directory(fs::path(p), ec);
}

// Extract an asset into a fresh staging dir created INSIDE `workDir` (so it is
// same-volume) and return the staging path. The caller runs the planner over it
// and is responsible for nothing extra: a failed placement removes staging, a
// successful one consumes/removes it.
std::wstring ExtractToStaging(const char* asset, const std::wstring& workDir,
                              PipelinePlan& planOut) {
    const std::wstring src = AssetPath(asset).wstring();
    planOut = DetectFile(src);
    const std::wstring staging = CreateStagingDir(workDir);
    EXPECT_FALSE(staging.empty());
    LibarchiveExtractor ex;
    ExtractCallbacks cb;
    const auto r = ex.extractToStaging(planOut, src, staging, cb);
    EXPECT_EQ(r.status, ExtractStatus::Ok) << asset;
    return staging;
}

// Full extract + place of `asset` into `workDir`. Returns the placement result.
PlaceResult ExtractAndPlace(const char* asset, const std::wstring& workDir) {
    PipelinePlan plan;
    const std::wstring staging = ExtractToStaging(asset, workDir, plan);
    LayoutPlanner planner;
    return planner.place(staging, workDir, plan.stem, plan.kind);
}

}  // namespace

// --- Auto-rename utility (unit) ---------------------------------------------

TEST(MakeNonCollidingName, ReturnsBaseWhenFree) {
    ScopedTempDir tmp;
    EXPECT_EQ(MakeNonCollidingName(tmp.path(), L"report.pdf", true),
              L"report.pdf");
    EXPECT_EQ(MakeNonCollidingName(tmp.path(), L"folder", false), L"folder");
}

TEST(MakeNonCollidingName, FileCounterBeforeExtension) {
    ScopedTempDir tmp;
    { std::ofstream f(fs::path(tmp.path()) / L"report.pdf"); f << "x"; }
    EXPECT_EQ(MakeNonCollidingName(tmp.path(), L"report.pdf", true),
              L"report (1).pdf");
    { std::ofstream f(fs::path(tmp.path()) / L"report (1).pdf"); f << "x"; }
    EXPECT_EQ(MakeNonCollidingName(tmp.path(), L"report.pdf", true),
              L"report (2).pdf");
}

TEST(MakeNonCollidingName, FolderCounterAppended) {
    ScopedTempDir tmp;
    CreateDirectoryW(
        ToExtendedPath((fs::path(tmp.path()) / L"project").wstring()).c_str(),
        nullptr);
    EXPECT_EQ(MakeNonCollidingName(tmp.path(), L"project", false),
              L"project (1)");
}

TEST(MakeNonCollidingName, DotfileHasNoExtension) {
    ScopedTempDir tmp;
    { std::ofstream f(fs::path(tmp.path()) / L".gitignore"); f << "x"; }
    // A leading-dot name is treated as having no extension -> counter at end.
    EXPECT_EQ(MakeNonCollidingName(tmp.path(), L".gitignore", true),
              L".gitignore (1)");
}

// --- 1-entry rule: single file ----------------------------------------------

TEST(Layout, SingleFilePlacedDirectly) {
    ScopedTempDir work;
    const auto r = ExtractAndPlace("zip__single-file.zip", work.path());
    ASSERT_EQ(r.status, PlaceStatus::Ok);
    EXPECT_EQ(r.rootEntryCount, 1u);
    EXPECT_TRUE(Exists(work.path(), L"readme.txt"));
    // Returned path is the placed item itself.
    EXPECT_EQ(r.placedPath, (fs::path(work.path()) / L"readme.txt").wstring());
    // No wrapper folder named after the stem.
    EXPECT_FALSE(Exists(work.path(), L"zip__single-file"));
}

// --- 1-entry rule: single folder, NO double-wrap ----------------------------

TEST(Layout, SingleFolderNotDoubleWrapped) {
    ScopedTempDir work;
    const auto r = ExtractAndPlace("zip__single-folder.zip", work.path());
    ASSERT_EQ(r.status, PlaceStatus::Ok);
    EXPECT_EQ(r.rootEntryCount, 1u);
    // The inner folder ("project") is placed directly, keeping its OWN name.
    EXPECT_TRUE(IsDir((fs::path(work.path()) / L"project").wstring()));
    EXPECT_TRUE(Exists(work.path(), L"project\\README.md"));
    EXPECT_TRUE(Exists(work.path(), L"project\\main.py"));
    // Never project/project or stem/project.
    EXPECT_FALSE(Exists(work.path(), L"project\\project"));
    EXPECT_FALSE(Exists(work.path(), L"zip__single-folder"));
}

// --- 2+ entries wrapped in a folder named after the stem --------------------

TEST(Layout, MultiFilesWrappedInStem) {
    ScopedTempDir work;
    const auto r = ExtractAndPlace("zip__multi-files.zip", work.path());
    ASSERT_EQ(r.status, PlaceStatus::Ok);
    EXPECT_GE(r.rootEntryCount, 2u);
    EXPECT_TRUE(IsDir((fs::path(work.path()) / L"zip__multi-files").wstring()));
    EXPECT_EQ(r.placedPath,
              (fs::path(work.path()) / L"zip__multi-files").wstring());
}

TEST(Layout, WrappedFolderIsNotHidden) {
    // The 2+-entry path renames the hidden staging dir into place; the placed
    // wrapper folder must end up a normal, visible folder (no hidden/system).
    ScopedTempDir work;
    const auto r = ExtractAndPlace("zip__multi-files.zip", work.path());
    ASSERT_EQ(r.status, PlaceStatus::Ok);
    const DWORD attr = GetFileAttributesW(r.placedPath.c_str());
    ASSERT_NE(attr, INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM), 0u);
}

TEST(Layout, MultiFoldersWrappedInStem) {
    ScopedTempDir work;
    const auto r = ExtractAndPlace("zip__multi-folders.zip", work.path());
    ASSERT_EQ(r.status, PlaceStatus::Ok);
    EXPECT_GE(r.rootEntryCount, 2u);
    EXPECT_TRUE(
        IsDir((fs::path(work.path()) / L"zip__multi-folders").wstring()));
}

TEST(Layout, MixedEntriesWrappedInStem) {
    ScopedTempDir work;
    const auto r = ExtractAndPlace("zip__mixed.zip", work.path());
    ASSERT_EQ(r.status, PlaceStatus::Ok);
    EXPECT_GE(r.rootEntryCount, 2u);
    EXPECT_TRUE(IsDir((fs::path(work.path()) / L"zip__mixed").wstring()));
}

// --- macOS cruft: __MACOSX + real folder => 2+ entries => wrapped -----------

TEST(Layout, MacosxCountsAsMultipleEntriesAndWraps) {
    ScopedTempDir work;
    const auto r = ExtractAndPlace("edge-macosx.zip", work.path());
    ASSERT_EQ(r.status, PlaceStatus::Ok);
    EXPECT_GE(r.rootEntryCount, 2u);
    // Wrapped under the stem; the real folder and __MACOSX both live inside.
    EXPECT_TRUE(IsDir((fs::path(work.path()) / L"edge-macosx").wstring()));
    EXPECT_TRUE(Exists(work.path(), L"edge-macosx\\myproject"));
    EXPECT_TRUE(Exists(work.path(), L"edge-macosx\\__MACOSX"));
}

// --- Empty archive: NO output, NO wrapper, temp cleaned ---------------------

TEST(Layout, EmptyZipProducesNoOutputNoWrapper) {
    ScopedTempDir work;
    const auto r = ExtractAndPlace("edge-empty.zip", work.path());
    EXPECT_EQ(r.status, PlaceStatus::NoOutput);
    EXPECT_EQ(r.rootEntryCount, 0u);
    EXPECT_TRUE(r.placedPath.empty());
    // No wrapper folder created.
    EXPECT_FALSE(Exists(work.path(), L"edge-empty"));
    // Working dir is empty (only the staging dir might have existed; it's gone).
    std::error_code ec;
    std::size_t n = 0;
    for (auto it = fs::directory_iterator(fs::path(work.path()), ec);
         it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        ++n;
    }
    EXPECT_EQ(n, 0u);
}

TEST(Layout, EmptyTarProducesNoOutput) {
    ScopedTempDir work;
    const auto r = ExtractAndPlace("edge-empty.tar", work.path());
    EXPECT_EQ(r.status, PlaceStatus::NoOutput);
    EXPECT_FALSE(Exists(work.path(), L"edge-empty"));
}

// --- Single empty folder at root: unwrapped per the 1-entry rule ------------

TEST(Layout, SingleEmptyFolderUnwrapped) {
    ScopedTempDir work;
    const auto r = ExtractAndPlace("edge-single-empty-folder.zip", work.path());
    ASSERT_EQ(r.status, PlaceStatus::Ok);
    EXPECT_EQ(r.rootEntryCount, 1u);
    // The empty inner folder ("emptydir") is placed directly, not wrapped.
    EXPECT_TRUE(IsDir((fs::path(work.path()) / L"emptydir").wstring()));
    EXPECT_FALSE(Exists(work.path(), L"edge-single-empty-folder"));
}

// --- Duplicate path inside one archive: still one root entry ----------------

TEST(Layout, DuplicatePathPlacesSingleFile) {
    ScopedTempDir work;
    const auto r = ExtractAndPlace("edge-duplicate-path.zip", work.path());
    ASSERT_EQ(r.status, PlaceStatus::Ok);
    // The two "dup.txt" entries collapse to one on-disk file -> 1 root entry.
    EXPECT_EQ(r.rootEntryCount, 1u);
    EXPECT_TRUE(Exists(work.path(), L"dup.txt"));
}

// --- Collision: placing a FILE twice into the same working dir --------------

TEST(Layout, FileCollisionAutoRenamedNeverOverwrites) {
    ScopedTempDir work;
    const auto r1 = ExtractAndPlace("zip__single-file.zip", work.path());
    ASSERT_EQ(r1.status, PlaceStatus::Ok);
    EXPECT_FALSE(r1.renamed);
    EXPECT_TRUE(Exists(work.path(), L"readme.txt"));

    const auto r2 = ExtractAndPlace("zip__single-file.zip", work.path());
    ASSERT_EQ(r2.status, PlaceStatus::Ok);
    EXPECT_TRUE(r2.renamed);
    // Counter inserted before the extension; original NOT overwritten.
    EXPECT_TRUE(Exists(work.path(), L"readme.txt"));
    EXPECT_TRUE(Exists(work.path(), L"readme (1).txt"));
    EXPECT_EQ(r2.placedPath,
              (fs::path(work.path()) / L"readme (1).txt").wstring());
}

// --- Collision: placing a FOLDER twice into the same working dir ------------

TEST(Layout, FolderCollisionAutoRenamedNeverOverwrites) {
    ScopedTempDir work;
    const auto r1 = ExtractAndPlace("zip__single-folder.zip", work.path());
    ASSERT_EQ(r1.status, PlaceStatus::Ok);
    EXPECT_FALSE(r1.renamed);
    EXPECT_TRUE(IsDir((fs::path(work.path()) / L"project").wstring()));

    const auto r2 = ExtractAndPlace("zip__single-folder.zip", work.path());
    ASSERT_EQ(r2.status, PlaceStatus::Ok);
    EXPECT_TRUE(r2.renamed);
    // Counter appended at the end for folders; original tree intact.
    EXPECT_TRUE(IsDir((fs::path(work.path()) / L"project").wstring()));
    EXPECT_TRUE(IsDir((fs::path(work.path()) / L"project (1)").wstring()));
    EXPECT_TRUE(Exists(work.path(), L"project (1)\\README.md"));
    EXPECT_EQ(r2.placedPath,
              (fs::path(work.path()) / L"project (1)").wstring());
}

// --- Collision: wrapper-folder (2+ entries) placed twice --------------------

TEST(Layout, WrapperFolderCollisionAutoRenamed) {
    ScopedTempDir work;
    const auto r1 = ExtractAndPlace("zip__multi-files.zip", work.path());
    ASSERT_EQ(r1.status, PlaceStatus::Ok);
    const auto r2 = ExtractAndPlace("zip__multi-files.zip", work.path());
    ASSERT_EQ(r2.status, PlaceStatus::Ok);
    EXPECT_TRUE(r2.renamed);
    EXPECT_TRUE(IsDir((fs::path(work.path()) / L"zip__multi-files").wstring()));
    EXPECT_TRUE(
        IsDir((fs::path(work.path()) / L"zip__multi-files (1)").wstring()));
}

// --- Case-only collision (NTFS is case-insensitive) -------------------------

TEST(Layout, CaseOnlyCollisionIsAutoRenamed) {
    ScopedTempDir work;
    // Pre-create "README.TXT"; placing "readme.txt" must NOT overwrite it.
    { std::ofstream f(fs::path(work.path()) / L"README.TXT"); f << "orig"; }

    const auto r = ExtractAndPlace("zip__single-file.zip", work.path());
    ASSERT_EQ(r.status, PlaceStatus::Ok);
    EXPECT_TRUE(r.renamed);
    EXPECT_TRUE(Exists(work.path(), L"readme (1).txt"));
    // Original (case-variant) file untouched.
    std::ifstream in(fs::path(work.path()) / L"README.TXT");
    std::string got;
    std::getline(in, got);
    EXPECT_EQ(got, "orig");
}

// --- Placement is a same-volume rename, not a copy --------------------------

TEST(Layout, PlacementIsSameVolumeRename) {
    ScopedTempDir work;
    PipelinePlan plan;
    const std::wstring staging =
        ExtractToStaging("zip__single-folder.zip", work.path(), plan);
    // Staging and work share a volume -> placement is an O(1) MoveFileEx rename.
    EXPECT_TRUE(SameVolume(staging, work.path()));
    LayoutPlanner planner;
    const auto r = planner.place(staging, work.path(), plan.stem, plan.kind);
    ASSERT_EQ(r.status, PlaceStatus::Ok);
    // Staging consumed: nothing left behind in the working dir except the item.
    std::error_code ec;
    EXPECT_FALSE(fs::exists(fs::path(staging), ec));
}

// --- Bad input: missing staging dir -----------------------------------------

TEST(Layout, MissingStagingDirIsBadInput) {
    ScopedTempDir work;
    LayoutPlanner planner;
    const auto r = planner.place(L"C:\\nope-ae-does-not-exist-98765",
                                 work.path(), L"stem", Kind::Container);
    EXPECT_EQ(r.status, PlaceStatus::BadInput);
}
