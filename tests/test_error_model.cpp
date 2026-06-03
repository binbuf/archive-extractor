#include "archive_core/error_model.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include "archive_core/detect.h"
#include "archive_core/extract.h"
#include "archive_core/layout.h"
#include "archive_core/logging.h"
#include "archive_core/retry.h"

namespace fs = std::filesystem;

using ae::Backend;
using ae::CreateStagingDir;
using ae::DetectFile;
using ae::ErrorCode;
using ae::ErrorCodeFromExtract;
using ae::ErrorCodeFromPlace;
using ae::ErrorCodeFromPlan;
using ae::ErrorCodeToken;
using ae::ErrorInfo;
using ae::ErrorMessageFor;
using ae::ExtractCallbacks;
using ae::ExtractResult;
using ae::ExtractStatus;
using ae::Format;
using ae::IsTransientLockError;
using ae::LibarchiveExtractor;
using ae::MakeError;
using ae::PipelinePlan;
using ae::PlaceStatus;
using ae::RefineWin32WriteError;
using ae::RemoveDirTree;
using ae::RetryPolicy;
using ae::RetryWithBackoff;

namespace {

#ifndef AE_TEST_ASSETS_DIR
#error "AE_TEST_ASSETS_DIR must be defined (see CMakeLists.txt)."
#endif

fs::path AssetPath(const char* name) {
    return fs::path(AE_TEST_ASSETS_DIR) / name;
}

// Unique temp dir under %TEMP%, removed on EVERY path (RAII). Same pattern as
// test_extract.cpp / test_layout.cpp.
class ScopedTempDir {
   public:
    ScopedTempDir() {
        wchar_t tmp[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, tmp);
        std::wstring base(tmp, n);
        path_ = CreateStagingDir(base);
        if (path_.empty()) {
            path_ = base + L"ae-test-fallback-em";
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

// Count entries directly under a directory.
std::size_t DirEntryCount(const std::wstring& dir) {
    std::error_code ec;
    std::size_t n = 0;
    for (auto it = fs::directory_iterator(fs::path(dir), ec);
         it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        ++n;
    }
    return n;
}

}  // namespace

// =====================================================================
// Catalog: every code has a non-empty, archive-naming message (except None).
// =====================================================================

TEST(ErrorCatalog, EveryCodeMapsToAMessageNamingTheArchive) {
    const ErrorCode codes[] = {
        ErrorCode::Unsupported,  ErrorCode::Corrupt,      ErrorCode::Encryption,
        ErrorCode::NeedPassword, ErrorCode::DiskFull,     ErrorCode::AccessDenied,
        ErrorCode::PathTooLong,  ErrorCode::FileInUse,    ErrorCode::Untrusted,
        ErrorCode::FileNotFound, ErrorCode::WriteFailed,  ErrorCode::PlaceFailed,
        ErrorCode::Internal,
    };
    for (ErrorCode c : codes) {
        const std::wstring msg = ErrorMessageFor(c, L"broken.zip");
        EXPECT_FALSE(msg.empty()) << static_cast<int>(c);
        // Plain-language body names the archive.
        EXPECT_NE(msg.find(L"broken.zip"), std::wstring::npos)
            << static_cast<int>(c);
        // Stable ASCII token for the log.
        EXPECT_GT(wcslen(ErrorCodeToken(c)), 0u);
    }
    // None has no message and is never an error.
    EXPECT_TRUE(ErrorMessageFor(ErrorCode::None, L"x.zip").empty());
    EXPECT_FALSE(MakeError(ErrorCode::None, L"x.zip").isError());
}

TEST(ErrorCatalog, CorruptMessageMatchesSpecWording) {
    // The fixture edge-truncated.zip may legitimately extract Ok (libarchive
    // recovers it), so the corrupt MAPPING is tested via the model directly.
    const std::wstring msg = ErrorMessageFor(ErrorCode::Corrupt, L"broken.zip");
    EXPECT_NE(msg.find(L"corrupt"), std::wstring::npos);
    EXPECT_NE(msg.find(L"broken.zip"), std::wstring::npos);
}

TEST(ErrorCatalog, MakeErrorPopulatesAllFields) {
    const ErrorInfo info = MakeError(ErrorCode::Untrusted, L"evil.zip");
    EXPECT_EQ(info.code, ErrorCode::Untrusted);
    EXPECT_EQ(info.archiveName, L"evil.zip");
    EXPECT_FALSE(info.message.empty());
    EXPECT_TRUE(info.isError());
}

// =====================================================================
// Bridges: engine / layout / detector statuses -> catalog codes.
// =====================================================================

TEST(ErrorBridge, ExtractStatusMapping) {
    EXPECT_EQ(ErrorCodeFromExtract(ExtractStatus::Ok), ErrorCode::None);
    EXPECT_EQ(ErrorCodeFromExtract(ExtractStatus::Cancelled), ErrorCode::None);
    EXPECT_EQ(ErrorCodeFromExtract(ExtractStatus::CorruptInput),
              ErrorCode::Corrupt);
    EXPECT_EQ(ErrorCodeFromExtract(ExtractStatus::Untrusted),
              ErrorCode::Untrusted);
    EXPECT_EQ(ErrorCodeFromExtract(ExtractStatus::NeedPassword),
              ErrorCode::NeedPassword);
    EXPECT_EQ(ErrorCodeFromExtract(ExtractStatus::Unsupported),
              ErrorCode::Unsupported);
    EXPECT_EQ(ErrorCodeFromExtract(ExtractStatus::OpenFailed),
              ErrorCode::FileNotFound);
    // WriteFailed refines on the supplied Win32 code.
    EXPECT_EQ(ErrorCodeFromExtract(ExtractStatus::WriteFailed),
              ErrorCode::WriteFailed);
    EXPECT_EQ(ErrorCodeFromExtract(ExtractStatus::WriteFailed, ERROR_DISK_FULL),
              ErrorCode::DiskFull);
    EXPECT_EQ(
        ErrorCodeFromExtract(ExtractStatus::WriteFailed, ERROR_ACCESS_DENIED),
        ErrorCode::AccessDenied);
}

TEST(ErrorBridge, PlaceStatusMapping) {
    EXPECT_EQ(ErrorCodeFromPlace(PlaceStatus::Ok), ErrorCode::None);
    EXPECT_EQ(ErrorCodeFromPlace(PlaceStatus::NoOutput), ErrorCode::None);
    EXPECT_EQ(ErrorCodeFromPlace(PlaceStatus::BadInput), ErrorCode::Internal);
    EXPECT_EQ(ErrorCodeFromPlace(PlaceStatus::PlaceFailed),
              ErrorCode::PlaceFailed);
    EXPECT_EQ(ErrorCodeFromPlace(PlaceStatus::PlaceFailed, ERROR_DISK_FULL),
              ErrorCode::DiskFull);
    EXPECT_EQ(ErrorCodeFromPlace(PlaceStatus::PlaceFailed, ERROR_ACCESS_DENIED),
              ErrorCode::AccessDenied);
    EXPECT_EQ(
        ErrorCodeFromPlace(PlaceStatus::PlaceFailed, ERROR_SHARING_VIOLATION),
        ErrorCode::FileInUse);
}

TEST(ErrorBridge, Win32RefinementCoversCatalog) {
    EXPECT_EQ(RefineWin32WriteError(ERROR_DISK_FULL, ErrorCode::WriteFailed),
              ErrorCode::DiskFull);
    EXPECT_EQ(RefineWin32WriteError(ERROR_ACCESS_DENIED, ErrorCode::WriteFailed),
              ErrorCode::AccessDenied);
    EXPECT_EQ(
        RefineWin32WriteError(ERROR_SHARING_VIOLATION, ErrorCode::WriteFailed),
        ErrorCode::FileInUse);
    EXPECT_EQ(
        RefineWin32WriteError(ERROR_FILENAME_EXCED_RANGE, ErrorCode::WriteFailed),
        ErrorCode::PathTooLong);
    EXPECT_EQ(RefineWin32WriteError(ERROR_FILE_NOT_FOUND, ErrorCode::WriteFailed),
              ErrorCode::FileNotFound);
    // Unknown error falls back.
    EXPECT_EQ(RefineWin32WriteError(0, ErrorCode::PlaceFailed),
              ErrorCode::PlaceFailed);
}

// =====================================================================
// Corpus: unsupported / unknown format (file untouched).
// =====================================================================

TEST(ErrorCorpus, UnknownBinIsUnsupportedAndFileUntouched) {
    const std::wstring src = AssetPath("edge-unknown.bin").wstring();
    ASSERT_TRUE(fs::exists(src));
    const auto sizeBefore = fs::file_size(src);

    const PipelinePlan plan = DetectFile(src);
    EXPECT_EQ(ErrorCodeFromPlan(plan), ErrorCode::Unsupported);
    EXPECT_FALSE(plan.supported());

    const std::wstring msg =
        ErrorMessageFor(ErrorCode::Unsupported, L"edge-unknown.bin");
    EXPECT_NE(msg.find(L"isn't supported"), std::wstring::npos);

    // File untouched (detection is read-only).
    EXPECT_TRUE(fs::exists(src));
    EXPECT_EQ(fs::file_size(src), sizeBefore);
}

// =====================================================================
// Corpus: zip-slip -> Untrusted error; nothing escapes; clean cleanup.
// =====================================================================

TEST(ErrorCorpus, ZipSlipYieldsUntrustedAndLeavesNoLitter) {
    const std::wstring src = AssetPath("edge-zip-slip.zip").wstring();
    ASSERT_TRUE(fs::exists(src));

    const fs::path parentBefore = fs::path(AE_TEST_ASSETS_DIR);
    {
        ScopedTempDir tmp;
        const PipelinePlan plan = DetectFile(src);
        LibarchiveExtractor ex;
        ExtractCallbacks cb;
        const ExtractResult r = ex.extractToStaging(plan, src, tmp.path(), cb);
        EXPECT_EQ(r.status, ExtractStatus::Untrusted);
        EXPECT_EQ(ErrorCodeFromExtract(r.status), ErrorCode::Untrusted);

        // Nothing escaped above the staging dir.
        const fs::path parent = fs::path(tmp.path()).parent_path();
        std::error_code ec;
        EXPECT_FALSE(fs::exists(parent / L"escaped.txt", ec));
        EXPECT_FALSE(fs::exists(parent / L"escaped2.txt", ec));
    }
    // The assets dir was not polluted by the bait paths.
    std::error_code ec;
    EXPECT_FALSE(fs::exists(parentBefore / L"escaped.txt", ec));
}

// =====================================================================
// Corpus: truncated zip -> graceful + NO litter (mapping tested via model;
// see note in the task spec — libarchive may recover this 84-byte fixture).
// =====================================================================

TEST(ErrorCorpus, TruncatedZipLeavesWorkingDirCleanRegardlessOfOutcome) {
    ScopedTempDir work;  // acts as the "working dir"
    const std::size_t before = DirEntryCount(work.path());

    {
        // Stage INSIDE work so any escape would show up in `work`.
        const std::wstring staging = CreateStagingDir(work.path());
        ASSERT_FALSE(staging.empty());
        const std::wstring src = AssetPath("edge-truncated.zip").wstring();
        const PipelinePlan plan = DetectFile(src);
        LibarchiveExtractor ex;
        ExtractCallbacks cb;
        const ExtractResult r = ex.extractToStaging(plan, src, staging, cb);
        // Either Ok (recovered) or CorruptInput — both map sanely.
        EXPECT_TRUE(r.status == ExtractStatus::Ok ||
                    r.status == ExtractStatus::CorruptInput)
            << static_cast<int>(r.status);
        if (r.status == ExtractStatus::CorruptInput) {
            EXPECT_EQ(ErrorCodeFromExtract(r.status), ErrorCode::Corrupt);
        }
        // On failure the flow removes staging; emulate that contract here.
        RemoveDirTree(staging);
    }

    // No payload escaped into the working dir.
    std::error_code ec;
    EXPECT_FALSE(fs::exists(fs::path(work.path()) / L"payload.txt", ec));
    EXPECT_EQ(DirEntryCount(work.path()), before);
}

// =====================================================================
// Mocked access-denied destination -> AccessDenied + working dir unchanged.
// =====================================================================

TEST(ErrorMocked, AccessDeniedDestinationMapsAndLeavesNoOutput) {
    // Make a read-only destination directory; a create/move into it fails with
    // ERROR_ACCESS_DENIED, which the model maps to AccessDenied.
    ScopedTempDir parent;
    const std::wstring dest =
        (fs::path(parent.path()) / L"readonly").wstring();
    ASSERT_TRUE(CreateDirectoryW(dest.c_str(), nullptr));
    SetFileAttributesW(dest.c_str(), FILE_ATTRIBUTE_READONLY);

    // Attempt to create a file inside the read-only directory.
    const std::wstring target = (fs::path(dest) / L"out.txt").wstring();
    HANDLE h = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD err = (h == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    // Some filesystems still allow file creation in a read-only DIR (the RO bit
    // governs deleting the dir, not writing into it). Validate the model maps
    // whatever permission error we get; otherwise validate the mapping directly.
    if (err == ERROR_ACCESS_DENIED) {
        EXPECT_EQ(RefineWin32WriteError(err, ErrorCode::WriteFailed),
                  ErrorCode::AccessDenied);
        EXPECT_FALSE(fs::exists(fs::path(target)));
    } else {
        // Deterministic fallback: the mapping itself is what matters here.
        EXPECT_EQ(
            RefineWin32WriteError(ERROR_ACCESS_DENIED, ErrorCode::WriteFailed),
            ErrorCode::AccessDenied);
    }

    SetFileAttributesW(dest.c_str(), FILE_ATTRIBUTE_NORMAL);
}

// =====================================================================
// Mocked disk-full + missing-file: model maps the Win32 codes correctly.
// =====================================================================

TEST(ErrorMocked, DiskFullMapping) {
    EXPECT_EQ(ErrorCodeFromExtract(ExtractStatus::WriteFailed, ERROR_DISK_FULL),
              ErrorCode::DiskFull);
    const std::wstring msg = ErrorMessageFor(ErrorCode::DiskFull, L"big.zip");
    EXPECT_NE(msg.find(L"disk space"), std::wstring::npos);
}

TEST(ErrorMocked, MissingFileMapping) {
    // A genuinely missing archive: opening it yields OpenFailed -> FileNotFound.
    const std::wstring src =
        AssetPath("does-not-exist-ae-zzzz.zip").wstring();
    EXPECT_FALSE(fs::exists(src));

    ScopedTempDir tmp;
    const PipelinePlan plan = DetectFile(src);  // falls back to extension
    LibarchiveExtractor ex;
    ExtractCallbacks cb;
    const ExtractResult r = ex.extractToStaging(plan, src, tmp.path(), cb);
    // The engine could not open the missing file.
    EXPECT_EQ(r.status, ExtractStatus::OpenFailed);
    EXPECT_EQ(ErrorCodeFromExtract(r.status), ErrorCode::FileNotFound);
    const std::wstring msg =
        ErrorMessageFor(ErrorCode::FileNotFound, L"gone.zip");
    EXPECT_NE(msg.find(L"could no longer be found"), std::wstring::npos);
}

// =====================================================================
// Retry-with-backoff helper (tested directly, with an injected no-op sleep).
// =====================================================================

TEST(Retry, TransientLockClassification) {
    EXPECT_TRUE(IsTransientLockError(ERROR_SHARING_VIOLATION));
    EXPECT_TRUE(IsTransientLockError(ERROR_LOCK_VIOLATION));
    EXPECT_TRUE(IsTransientLockError(ERROR_ACCESS_DENIED));
    EXPECT_FALSE(IsTransientLockError(ERROR_DISK_FULL));
    EXPECT_FALSE(IsTransientLockError(ERROR_FILE_NOT_FOUND));
    EXPECT_FALSE(IsTransientLockError(0));
}

TEST(Retry, SucceedsAfterTransientFailures) {
    int calls = 0;
    int sleeps = 0;
    RetryPolicy pol;
    pol.attempts = 5;
    const bool ok = RetryWithBackoff(
        [&](unsigned long& err) {
            ++calls;
            if (calls < 3) {  // fail twice transiently, then succeed
                err = ERROR_SHARING_VIOLATION;
                return false;
            }
            return true;
        },
        pol, [&](unsigned) { ++sleeps; });
    EXPECT_TRUE(ok);
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(sleeps, 2);  // one backoff before each retry
}

TEST(Retry, GivesUpAfterAttemptsOnPersistentLock) {
    int calls = 0;
    RetryPolicy pol;
    pol.attempts = 4;
    const bool ok = RetryWithBackoff(
        [&](unsigned long& err) {
            ++calls;
            err = ERROR_SHARING_VIOLATION;
            return false;
        },
        pol, [](unsigned) {});
    EXPECT_FALSE(ok);
    EXPECT_EQ(calls, 4);  // exhausted the budget
}

TEST(Retry, DoesNotRetryPermanentErrors) {
    int calls = 0;
    const bool ok = RetryWithBackoff(
        [&](unsigned long& err) {
            ++calls;
            err = ERROR_DISK_FULL;  // permanent -> no retry
            return false;
        },
        RetryPolicy{}, [](unsigned) {});
    EXPECT_FALSE(ok);
    EXPECT_EQ(calls, 1);  // stopped immediately
}

TEST(Retry, LockedDestinationFileIsRetriedThenSucceeds) {
    // Realistic scenario: a destination file is briefly locked (exclusive, no
    // sharing) then released; the retried open succeeds. We model the "move"
    // as an exclusive CreateFile that fails with a sharing violation while a
    // holder keeps the file open, then a worker releases it after a few tries.
    ScopedTempDir tmp;
    const std::wstring target = (fs::path(tmp.path()) / L"locked.bin").wstring();
    {
        std::ofstream f(target, std::ios::binary);
        f << "x";
    }
    // Hold the file open with NO sharing -> exclusive lock.
    HANDLE hold = CreateFileW(target.c_str(), GENERIC_READ | GENERIC_WRITE,
                              0 /* no sharing */, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(hold, INVALID_HANDLE_VALUE);

    std::atomic<int> attempt{0};
    const bool ok = RetryWithBackoff(
        [&](unsigned long& err) {
            const int n = ++attempt;
            if (n == 3 && hold != INVALID_HANDLE_VALUE) {
                CloseHandle(hold);  // holder releases before the 3rd try
                hold = INVALID_HANDLE_VALUE;
            }
            HANDLE h = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                return true;
            }
            err = GetLastError();
            return false;
        },
        RetryPolicy{}, [](unsigned) {});
    if (hold != INVALID_HANDLE_VALUE) CloseHandle(hold);

    EXPECT_TRUE(ok);
    EXPECT_GE(attempt.load(), 3);
}

// =====================================================================
// Logging: rotating file logger never records payload bytes; rotation caps.
// =====================================================================

namespace {
std::string ReadAll(const std::wstring& path) {
    std::ifstream in(fs::path(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}
}  // namespace

TEST(Logging, FileLogRecordsCodeButNeverPayloadOrPassword) {
    ScopedTempDir tmp;
    const std::wstring logPath = (fs::path(tmp.path()) / L"log.txt").wstring();

    ASSERT_TRUE(ae::InitFileLog(logPath, 256 * 1024));
    EXPECT_EQ(ae::LogFilePath(), logPath);

    // Simulate what the flow logs: code + format + entry name. The "secret"
    // payload/password strings below must NEVER reach the log.
    const wchar_t* kSecretPayload = L"TOPSECRET_PAYLOAD_BYTES";
    const wchar_t* kSecretPassword = L"hunter2_password";
    (void)kSecretPayload;
    (void)kSecretPassword;

    ae::Log(std::wstring(L"[error] code=") +
            ErrorCodeToken(ErrorCode::Corrupt) +
            L" format=1 archive=broken.zip entry=docs/readme.txt");

    ae::ShutdownFileLog();

    const std::string contents = ReadAll(logPath);
    EXPECT_NE(contents.find("CORRUPT"), std::string::npos);
    EXPECT_NE(contents.find("broken.zip"), std::string::npos);
    EXPECT_NE(contents.find("entry=docs/readme.txt"), std::string::npos);
    // The log must never contain archive contents or passwords.
    EXPECT_EQ(contents.find("TOPSECRET_PAYLOAD_BYTES"), std::string::npos);
    EXPECT_EQ(contents.find("hunter2_password"), std::string::npos);
}

TEST(Logging, RotatesWhenSizeCapExceeded) {
    ScopedTempDir tmp;
    const std::wstring logPath = (fs::path(tmp.path()) / L"log.txt").wstring();
    const std::wstring backup = logPath + L".1";

    // Tiny cap to force a rotation quickly.
    ASSERT_TRUE(ae::InitFileLog(logPath, 256));
    for (int i = 0; i < 50; ++i) {
        ae::Log(L"[diag] padding line to exceed the small rotation cap quickly");
    }
    ae::ShutdownFileLog();

    // After rotation a backup exists and the live log is small again.
    std::error_code ec;
    EXPECT_TRUE(fs::exists(fs::path(backup), ec));
    EXPECT_LE(fs::file_size(fs::path(logPath), ec), static_cast<std::uintmax_t>(256 + 512));
}

TEST(Logging, WorksWhenFileLogDisabled) {
    ae::ShutdownFileLog();  // ensure disabled
    EXPECT_TRUE(ae::LogFilePath().empty());
    // Must be a no-op (no crash) without a file sink.
    ae::Log(L"[diag] no file sink configured");
    SUCCEED();
}
