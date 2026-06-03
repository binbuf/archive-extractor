#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "archive_core/cli.h"
#include "archive_core/detect.h"
#include "archive_core/extract.h"
#include "archive_core/sanitize.h"

namespace fs = std::filesystem;

using ae::BombVerdict;
using ae::CommandLine;
using ae::CreateStagingDir;
using ae::DetectFile;
using ae::EvaluateBombGuard;
using ae::ExtractCallbacks;
using ae::ExtractResult;
using ae::ExtractStatus;
using ae::InnerNameRegistry;
using ae::IsReservedDeviceName;
using ae::LibarchiveExtractor;
using ae::LinkPolicy;
using ae::Mode;
using ae::ParseCommandLine;
using ae::PipelinePlan;
using ae::RemoveDirTree;
using ae::SanitizeInnerSegment;

namespace {

#ifndef AE_TEST_ASSETS_DIR
#error "AE_TEST_ASSETS_DIR must be defined (see CMakeLists.txt)."
#endif

fs::path AssetPath(const char* name) {
    return fs::path(AE_TEST_ASSETS_DIR) / name;
}

// Unique temp dir, deleted on destruction on every path (mirrors test_extract).
class ScopedTempDir {
   public:
    ScopedTempDir() {
        wchar_t tmp[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, tmp);
        std::wstring base(tmp, n);
        path_ = CreateStagingDir(base);
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

std::string ReadBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

ExtractResult Extract(const char* asset, const std::wstring& staging,
                      const ExtractCallbacks& cb) {
    const std::wstring src = AssetPath(asset).wstring();
    const PipelinePlan plan = DetectFile(src);
    LibarchiveExtractor ex;
    return ex.extractToStaging(plan, src, staging, cb);
}

}  // namespace

// --- Reserved device names --------------------------------------------------

TEST(Sanitize, ReservedDeviceNames) {
    for (const wchar_t* r :
         {L"CON", L"con", L"PRN", L"AUX", L"NUL", L"COM1", L"COM9", L"LPT1",
          L"LPT9", L"con.txt", L"AUX.dat", L"CONIN$", L"CONOUT$"}) {
        EXPECT_TRUE(IsReservedDeviceName(r)) << "expected reserved: ";
    }
    for (const wchar_t* ok :
         {L"COM0", L"LPT0", L"COM10", L"CONSOLE", L"README", L"contacts",
          L"lpt", L"com"}) {
        EXPECT_FALSE(IsReservedDeviceName(ok)) << "expected not reserved";
    }
}

// --- Segment sanitization ---------------------------------------------------

TEST(Sanitize, SegmentRules) {
    EXPECT_EQ(SanitizeInnerSegment(L"CON.txt"), L"CON_.txt");
    EXPECT_EQ(SanitizeInnerSegment(L"AUX"), L"AUX_");
    EXPECT_EQ(SanitizeInnerSegment(L"bad<name>.txt"), L"bad_name_.txt");
    EXPECT_EQ(SanitizeInnerSegment(L"a:b|c?.txt"), L"a_b_c_.txt");
    EXPECT_EQ(SanitizeInnerSegment(L"trailingdot."), L"trailingdot");
    EXPECT_EQ(SanitizeInnerSegment(L"trailing space  "), L"trailing space");
    EXPECT_EQ(SanitizeInnerSegment(L"normal.txt"), L"normal.txt");
    // A segment that fully sanitizes away never returns empty.
    EXPECT_EQ(SanitizeInnerSegment(L"   "), L"_");
    EXPECT_EQ(SanitizeInnerSegment(L"..."), L"_");
}

// --- Case-collision / duplicate registry ------------------------------------

TEST(Sanitize, RegistryCaseCollisionAndDuplicate) {
    InnerNameRegistry reg;
    InnerNameRegistry::Disposition d;

    EXPECT_EQ(reg.Resolve(L"README", false, d), L"README");
    EXPECT_EQ(d, InnerNameRegistry::Disposition::Fresh);

    // Case-only collision: renamed with a " (n)" leaf suffix.
    EXPECT_EQ(reg.Resolve(L"readme", false, d), L"readme (1)");
    EXPECT_EQ(d, InnerNameRegistry::Disposition::Renamed);

    // Exact duplicate path: last-wins (same path returned).
    EXPECT_EQ(reg.Resolve(L"README", false, d), L"README");
    EXPECT_EQ(d, InnerNameRegistry::Disposition::Duplicate);

    // Extension is preserved and the newcomer keeps its own case when the
    // counter is inserted.
    EXPECT_EQ(reg.Resolve(L"a\\file.txt", false, d), L"a\\file.txt");
    EXPECT_EQ(reg.Resolve(L"a\\FILE.txt", false, d), L"a\\FILE (1).txt");
    EXPECT_EQ(d, InnerNameRegistry::Disposition::Renamed);

    // Directories that case-fold to an existing one MERGE (reuse canonical path).
    EXPECT_EQ(reg.Resolve(L"Dir", true, d), L"Dir");
    EXPECT_EQ(reg.Resolve(L"dir", true, d), L"Dir");
    EXPECT_EQ(d, InnerNameRegistry::Disposition::Merged);
}

// --- Decompression-bomb guard -----------------------------------------------

TEST(BombGuard, RatioAndSpaceTrips) {
    const std::uint64_t GiB = 1024ull * 1024 * 1024;

    // Small output, absurd ratio: below the floor -> not tripped.
    BombVerdict v = EvaluateBombGuard(1024, 1024ull * 1024, /*free=*/100 * GiB);
    EXPECT_FALSE(v.tripped);

    // Large output, absurd ratio -> ratio trip.
    v = EvaluateBombGuard(/*compressed=*/1024 * 1024, /*written=*/2 * GiB,
                          /*free=*/100 * GiB);
    EXPECT_TRUE(v.tripped);
    EXPECT_TRUE(v.ratioTrip);

    // Output approaching free space -> space trip (even with a sane ratio).
    v = EvaluateBombGuard(/*compressed=*/GiB, /*written=*/GiB,
                          /*free=*/GiB + 1024 * 1024);
    EXPECT_TRUE(v.tripped);
    EXPECT_TRUE(v.spaceTrip);

    // Ordinary, modest extraction -> not tripped.
    v = EvaluateBombGuard(/*compressed=*/10 * 1024 * 1024,
                          /*written=*/50 * 1024 * 1024, /*free=*/100 * GiB);
    EXPECT_FALSE(v.tripped);

    // Unknown free space (0) skips the space check; ratio still applies.
    v = EvaluateBombGuard(/*compressed=*/0, /*written=*/2 * GiB, /*free=*/0);
    EXPECT_FALSE(v.tripped);  // compressed==0 -> no ratio either
}

// --- Illegal / reserved / case-colliding names (edge-illegal-names.zip) -----

TEST(EdgeExtract, IllegalNamesSanitized) {
    ScopedTempDir tmp;
    ExtractCallbacks cb;
    const auto r = Extract("edge-illegal-names.zip", tmp.path(), cb);
    ASSERT_EQ(r.status, ExtractStatus::Ok);

    EXPECT_TRUE(Exists(tmp.path(), L"CON_.txt"));
    EXPECT_TRUE(Exists(tmp.path(), L"AUX_"));
    EXPECT_TRUE(Exists(tmp.path(), L"bad_name_.txt"));
    EXPECT_TRUE(Exists(tmp.path(), L"trailingdot"));
    EXPECT_TRUE(Exists(tmp.path(), L"README"));
    // The case-only collision keeps both, renaming the second.
    EXPECT_TRUE(Exists(tmp.path(), L"readme (1)"));
    // (Win32 silently strips a trailing dot when resolving a path, so probing
    // "trailingdot." would resolve to "trailingdot" — not a meaningful check.
    // The point is that the sanitized "trailingdot" file exists, asserted above.)
}

// --- Duplicate path: last-wins (edge-duplicate-path.zip) --------------------

TEST(EdgeExtract, DuplicatePathLastWins) {
    ScopedTempDir tmp;
    ExtractCallbacks cb;
    const auto r = Extract("edge-duplicate-path.zip", tmp.path(), cb);
    ASSERT_EQ(r.status, ExtractStatus::Ok);
    ASSERT_TRUE(Exists(tmp.path(), L"dup.txt"));
    const std::string got = ReadBytes(fs::path(tmp.path()) / L"dup.txt");
    EXPECT_EQ(got, "SECOND copy of dup.txt (last-wins)\n");
}

// --- macOS cruft preserved as-is (edge-macosx.zip) --------------------------

TEST(EdgeExtract, MacOsxCruftPreserved) {
    ScopedTempDir tmp;
    ExtractCallbacks cb;
    const auto r = Extract("edge-macosx.zip", tmp.path(), cb);
    ASSERT_EQ(r.status, ExtractStatus::Ok);
    EXPECT_TRUE(Exists(tmp.path(), L"__MACOSX"));
    EXPECT_TRUE(Exists(tmp.path(), L".DS_Store"));
    EXPECT_TRUE(Exists(tmp.path(), L"myproject\\main.py"));
}

// --- Links: Copy policy materializes targets, external skipped --------------

TEST(EdgeExtract, LinksCopyPolicyCopiesTargets) {
    ScopedTempDir tmp;
    ExtractCallbacks cb;
    cb.requestLinkPolicy = []() { return LinkPolicy::Copy; };
    const auto r = Extract("edge-links.tar", tmp.path(), cb);
    ASSERT_EQ(r.status, ExtractStatus::Ok);
    EXPECT_TRUE(r.hadLinks);
    EXPECT_EQ(r.linksCopied, 2u);    // sym + hard -> readme.txt
    EXPECT_GE(r.entriesSkipped, 1u);  // the /etc/passwd external link

    EXPECT_TRUE(Exists(tmp.path(), L"readme.txt"));
    const std::string real = ReadBytes(fs::path(tmp.path()) / L"readme.txt");
    EXPECT_TRUE(Exists(tmp.path(), L"link-to-readme.txt"));
    EXPECT_EQ(ReadBytes(fs::path(tmp.path()) / L"link-to-readme.txt"), real);
    EXPECT_TRUE(Exists(tmp.path(), L"hardlink-to-readme.txt"));
    EXPECT_EQ(ReadBytes(fs::path(tmp.path()) / L"hardlink-to-readme.txt"), real);
    // The external target was not copied.
    EXPECT_FALSE(Exists(tmp.path(), L"link-to-outside"));
}

TEST(EdgeExtract, LinksSkipPolicyIsDefault) {
    ScopedTempDir tmp;
    ExtractCallbacks cb;  // no requestLinkPolicy -> default Skip
    const auto r = Extract("edge-links.tar", tmp.path(), cb);
    ASSERT_EQ(r.status, ExtractStatus::Ok);
    EXPECT_TRUE(r.hadLinks);
    EXPECT_EQ(r.linksCopied, 0u);
    EXPECT_TRUE(Exists(tmp.path(), L"readme.txt"));
    EXPECT_FALSE(Exists(tmp.path(), L"link-to-readme.txt"));
}

// --- Launch & argument edge cases -------------------------------------------

TEST(LaunchArgs, MultiplePathsCollectExtras) {
    const CommandLine cl = ParseCommandLine({L"a.zip", L"b.7z", L"c.tar"});
    EXPECT_EQ(cl.mode, Mode::Extract);
    EXPECT_EQ(cl.archivePath, L"a.zip");
    ASSERT_EQ(cl.extraPaths.size(), 2u);
    EXPECT_EQ(cl.extraPaths[0], L"b.7z");
    EXPECT_EQ(cl.extraPaths[1], L"c.tar");
}

TEST(LaunchArgs, NoArgumentIsNoOp) {
    const CommandLine cl = ParseCommandLine({});
    EXPECT_EQ(cl.mode, Mode::None);
    EXPECT_TRUE(cl.archivePath.empty());
    EXPECT_TRUE(cl.extraPaths.empty());
}

TEST(LaunchArgs, RegisterShortCircuitsBeforePaths) {
    const CommandLine cl = ParseCommandLine({L"--register", L"a.zip"});
    EXPECT_EQ(cl.mode, Mode::Register);
    EXPECT_TRUE(cl.archivePath.empty());
}

TEST(LaunchArgs, SetDefaultTakesOneSampleNoExtras) {
    const CommandLine cl =
        ParseCommandLine({L"--set-default", L"a.zip", L"b.zip"});
    EXPECT_EQ(cl.mode, Mode::SetDefault);
    EXPECT_EQ(cl.archivePath, L"a.zip");
    EXPECT_TRUE(cl.extraPaths.empty());
}
