// Tests for the full password flow (task 10): encrypted zip via libarchive
// (ZipCrypto + WinZip-AES), encrypted 7z via the 7z.dll backend (clear-header +
// -mhe encrypted-header), and encrypted RAR (gated on rar.exe-generated assets).
// Covers wrong-password retry-then-success, retry-cap-then-clean-abort, cancel
// at the prompt, the no-callback NeedPassword case, the mixed/all-encrypted zip,
// and the security requirements (the password never reaches the log, and an
// aborted attempt leaves no decompressed output).
//
// These exercise the ENGINE password contract headlessly (ExtractCallbacks::
// requestPassword); the Win32 prompt dialog is wired separately in the app.
// Every test extracts into a UNIQUE temp dir deleted on every path (RAII).

#include "archive_core/extract.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "archive_core/detect.h"
#include "archive_core/logging.h"

namespace fs = std::filesystem;

using ae::Backend;
using ae::CreateStagingDir;
using ae::DetectFile;
using ae::ExtractCallbacks;
using ae::ExtractResult;
using ae::ExtractStatus;
using ae::IExtractor;
using ae::kMaxPasswordAttempts;
using ae::LibarchiveExtractor;
using ae::PasswordPrompt;
using ae::PipelinePlan;
using ae::RemoveDirTree;
using ae::ResolveBackend;
using ae::SevenZipExtractor;
using ae::ToExtendedPath;

namespace {

#ifndef AE_TEST_ASSETS_DIR
#error "AE_TEST_ASSETS_DIR must be defined (see CMakeLists.txt)."
#endif

constexpr const wchar_t* kPassword = L"test1234";
constexpr const wchar_t* kWrongPassword = L"nope-not-it";

fs::path AssetPath(const char* name) {
    return fs::path(AE_TEST_ASSETS_DIR) / name;
}

bool AssetExists(const char* name) {
    std::error_code ec;
    return fs::exists(AssetPath(name), ec);
}

// Unique hidden temp dir under %TEMP%, removed on destruction (RAII).
class ScopedTempDir {
   public:
    ScopedTempDir() {
        wchar_t tmp[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, tmp);
        std::wstring base(tmp, n);
        path_ = CreateStagingDir(base);
        if (path_.empty()) {
            path_ = base + L"ae-pw-test-fallback";
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

// Route + run the right backend exactly like the production flow: detect, then
// resolve the encryption re-route, then pick libarchive vs the 7z.dll backend.
ExtractResult RunRouted(const char* asset, const std::wstring& staging,
                        const ExtractCallbacks& cb) {
    const std::wstring src = AssetPath(asset).wstring();
    PipelinePlan plan = DetectFile(src);
    const std::wstring source =
        plan.firstPartPath.empty() ? src : plan.firstPartPath;
    plan.backend = ResolveBackend(plan, source);
    LibarchiveExtractor lib;
    SevenZipExtractor sevenZip;
    IExtractor& ex = (plan.backend == Backend::SevenZipDll)
                         ? static_cast<IExtractor&>(sevenZip)
                         : static_cast<IExtractor&>(lib);
    return ex.extractToStaging(plan, source, staging, cb);
}

// --- Password callback factories -------------------------------------------

// Always returns the given password; records the number of prompts.
ExtractCallbacks AlwaysReturn(const wchar_t* pw, std::atomic<int>* prompts) {
    ExtractCallbacks cb;
    std::wstring s = pw;
    cb.requestPassword =
        [s, prompts](const PasswordPrompt&) -> std::optional<std::wstring> {
        if (prompts) ++*prompts;
        return s;
    };
    return cb;
}

// Returns the wrong password on the first attempt(s), then the correct one once
// `attempt >= wrongCount`. Records prompts and the highest attempt seen.
ExtractCallbacks WrongThenCorrect(unsigned wrongCount, std::atomic<int>* prompts,
                                  std::atomic<int>* maxAttempt) {
    ExtractCallbacks cb;
    cb.requestPassword = [wrongCount, prompts, maxAttempt](
                             const PasswordPrompt& p)
        -> std::optional<std::wstring> {
        if (prompts) ++*prompts;
        if (maxAttempt &&
            static_cast<int>(p.attempt) > maxAttempt->load()) {
            maxAttempt->store(static_cast<int>(p.attempt));
        }
        return p.attempt < wrongCount ? std::wstring(kWrongPassword)
                                      : std::wstring(kPassword);
    };
    return cb;
}

// Cancels (returns nullopt) immediately at the prompt.
ExtractCallbacks CancelAtPrompt(std::atomic<int>* prompts) {
    ExtractCallbacks cb;
    cb.requestPassword =
        [prompts](const PasswordPrompt&) -> std::optional<std::wstring> {
        if (prompts) ++*prompts;
        return std::nullopt;
    };
    return cb;
}

}  // namespace

// ===========================================================================
// Encrypted zip via libarchive (ZipCrypto + WinZip-AES)
// ===========================================================================

TEST(Passwords, ZipCryptoExtractsWithPassword) {
    ScopedTempDir tmp;
    std::atomic<int> prompts{0};
    auto cb = AlwaysReturn(kPassword, &prompts);
    const auto r = RunRouted("zip-zipcrypto__password.zip", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Ok) << r.message.c_str();
    EXPECT_GE(CountFiles(tmp.path()), 1u);
    EXPECT_GE(prompts.load(), 1);  // prompted at least once
}

TEST(Passwords, ZipAesExtractsWithPassword) {
    if (!AssetExists("zip-aes__password.zip")) {
        GTEST_SKIP() << "zip-aes asset absent (pyzipper unavailable at gen time)";
    }
    ScopedTempDir tmp;
    std::atomic<int> prompts{0};
    auto cb = AlwaysReturn(kPassword, &prompts);
    const auto r = RunRouted("zip-aes__password.zip", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Ok) << r.message.c_str();
    EXPECT_GE(CountFiles(tmp.path()), 1u);
}

// The "mixed" structure asset (files + folders at the root) all-encrypted:
// one accepted password decrypts every entry — never re-prompted per file.
TEST(Passwords, MixedZipExtractsWithSinglePassword) {
    ScopedTempDir tmp;
    std::atomic<int> prompts{0};
    auto cb = AlwaysReturn(kPassword, &prompts);
    const auto r = RunRouted("zip-zipcrypto__password.zip", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Ok) << r.message.c_str();
    EXPECT_GE(CountFiles(tmp.path()), 2u);
    // The cached passphrase is reused: a single archive prompts exactly once.
    EXPECT_EQ(prompts.load(), 1);
}

TEST(Passwords, ZipNeedsPasswordWhenNoCallback) {
    ScopedTempDir tmp;
    ExtractCallbacks cb;  // no requestPassword hook
    const auto r = RunRouted("zip-zipcrypto__password.zip", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::NeedPassword);
    EXPECT_EQ(CountFiles(tmp.path()), 0u);  // nothing decrypted
}

TEST(Passwords, ZipWrongThenCorrectRetrySucceeds) {
    ScopedTempDir tmp;
    std::atomic<int> prompts{0};
    std::atomic<int> maxAttempt{0};
    auto cb = WrongThenCorrect(/*wrongCount=*/1, &prompts, &maxAttempt);
    const auto r = RunRouted("zip-zipcrypto__password.zip", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Ok) << r.message.c_str();
    EXPECT_GE(CountFiles(tmp.path()), 1u);
    EXPECT_GE(prompts.load(), 2);       // first wrong, then correct
    EXPECT_GE(maxAttempt.load(), 1);    // a retry (attempt index >= 1) occurred
}

TEST(Passwords, ZipRetryCapAbortsCleanly) {
    ScopedTempDir tmp;
    std::atomic<int> prompts{0};
    auto cb = AlwaysReturn(kWrongPassword, &prompts);
    const auto r = RunRouted("zip-zipcrypto__password.zip", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::NeedPassword);
    EXPECT_EQ(CountFiles(tmp.path()), 0u);  // aborted: no decompressed output
    EXPECT_EQ(prompts.load(), static_cast<int>(kMaxPasswordAttempts));
}

TEST(Passwords, ZipCancelAtPromptAborts) {
    ScopedTempDir tmp;
    std::atomic<int> prompts{0};
    auto cb = CancelAtPrompt(&prompts);
    const auto r = RunRouted("zip-zipcrypto__password.zip", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Cancelled);
    EXPECT_EQ(CountFiles(tmp.path()), 0u);
    EXPECT_EQ(prompts.load(), 1);  // cancelled at the very first prompt
}

// ===========================================================================
// Encrypted 7z via the 7z.dll backend (clear-header + encrypted-header)
// ===========================================================================

TEST(Passwords, SevenZipExtractsWithPassword) {
    ScopedTempDir tmp;
    std::atomic<int> prompts{0};
    auto cb = AlwaysReturn(kPassword, &prompts);
    const auto r = RunRouted("7z-aes__password.7z", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Ok) << r.message.c_str();
    EXPECT_GE(CountFiles(tmp.path()), 1u);
}

TEST(Passwords, SevenZipWrongThenCorrectRetrySucceeds) {
    ScopedTempDir tmp;
    std::atomic<int> prompts{0};
    std::atomic<int> maxAttempt{0};
    auto cb = WrongThenCorrect(/*wrongCount=*/1, &prompts, &maxAttempt);
    const auto r = RunRouted("7z-aes__password.7z", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Ok) << r.message.c_str();
    EXPECT_GE(CountFiles(tmp.path()), 1u);
    EXPECT_GE(prompts.load(), 2);
}

TEST(Passwords, SevenZipRetryCapAbortsCleanly) {
    ScopedTempDir tmp;
    std::atomic<int> prompts{0};
    auto cb = AlwaysReturn(kWrongPassword, &prompts);
    const auto r = RunRouted("7z-aes__password.7z", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::NeedPassword);
    EXPECT_EQ(CountFiles(tmp.path()), 0u);
    EXPECT_EQ(prompts.load(), static_cast<int>(kMaxPasswordAttempts));
}

TEST(Passwords, SevenZipCancelAtPromptAborts) {
    ScopedTempDir tmp;
    std::atomic<int> prompts{0};
    auto cb = CancelAtPrompt(&prompts);
    const auto r = RunRouted("7z-aes__password.7z", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Cancelled);
    EXPECT_EQ(CountFiles(tmp.path()), 0u);
}

TEST(Passwords, SevenZipHeaderEncryptedExtractsWithPassword) {
    ScopedTempDir tmp;
    std::atomic<int> prompts{0};
    auto cb = AlwaysReturn(kPassword, &prompts);
    const auto r = RunRouted("7z-aes-header__password.7z", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Ok) << r.message.c_str();
    EXPECT_GE(CountFiles(tmp.path()), 1u);
}

TEST(Passwords, SevenZipHeaderEncryptedWrongThenCorrect) {
    ScopedTempDir tmp;
    std::atomic<int> prompts{0};
    std::atomic<int> maxAttempt{0};
    auto cb = WrongThenCorrect(/*wrongCount=*/1, &prompts, &maxAttempt);
    const auto r = RunRouted("7z-aes-header__password.7z", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Ok) << r.message.c_str();
    EXPECT_GE(CountFiles(tmp.path()), 1u);
    EXPECT_GE(prompts.load(), 2);
}

TEST(Passwords, SevenZipHeaderEncryptedRetryCapAborts) {
    ScopedTempDir tmp;
    std::atomic<int> prompts{0};
    auto cb = AlwaysReturn(kWrongPassword, &prompts);
    const auto r = RunRouted("7z-aes-header__password.7z", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::NeedPassword);
    EXPECT_EQ(CountFiles(tmp.path()), 0u);
    EXPECT_EQ(prompts.load(), static_cast<int>(kMaxPasswordAttempts));
}

TEST(Passwords, SevenZipHeaderEncryptedCancelAborts) {
    ScopedTempDir tmp;
    std::atomic<int> prompts{0};
    auto cb = CancelAtPrompt(&prompts);
    const auto r = RunRouted("7z-aes-header__password.7z", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Cancelled);
    EXPECT_EQ(CountFiles(tmp.path()), 0u);
}

// ===========================================================================
// Encrypted RAR (gated on rar.exe-generated assets; SKIP when absent)
// ===========================================================================

TEST(Passwords, RarEncryptedExtractsWithPassword) {
    if (!AssetExists("rar-aes__password.rar")) {
        GTEST_SKIP() << "rar-aes asset absent (no rar.exe in corpus generator)";
    }
    ScopedTempDir tmp;
    std::atomic<int> prompts{0};
    auto cb = AlwaysReturn(kPassword, &prompts);
    const auto r = RunRouted("rar-aes__password.rar", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Ok) << r.message.c_str();
    EXPECT_GE(CountFiles(tmp.path()), 1u);
}

TEST(Passwords, RarHeaderEncryptedExtractsWithPassword) {
    if (!AssetExists("rar-aes-header__password.rar")) {
        GTEST_SKIP() << "rar-aes-header asset absent (no rar.exe in generator)";
    }
    ScopedTempDir tmp;
    std::atomic<int> prompts{0};
    auto cb = AlwaysReturn(kPassword, &prompts);
    const auto r = RunRouted("rar-aes-header__password.rar", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::Ok) << r.message.c_str();
    EXPECT_GE(CountFiles(tmp.path()), 1u);
}

TEST(Passwords, RarEncryptedRetryCapAborts) {
    if (!AssetExists("rar-aes__password.rar")) {
        GTEST_SKIP() << "rar-aes asset absent (no rar.exe in corpus generator)";
    }
    ScopedTempDir tmp;
    std::atomic<int> prompts{0};
    auto cb = AlwaysReturn(kWrongPassword, &prompts);
    const auto r = RunRouted("rar-aes__password.rar", tmp.path(), cb);
    EXPECT_EQ(r.status, ExtractStatus::NeedPassword);
    EXPECT_EQ(CountFiles(tmp.path()), 0u);
}

// ===========================================================================
// Security: the password never reaches the diagnostic log
// ===========================================================================

namespace {

std::string ReadAllBytes(const std::wstring& path) {
    HANDLE h = CreateFileW(ToExtendedPath(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) {
        out.append(buf, got);
    }
    CloseHandle(h);
    return out;
}

// A temp log file routed through the real logger, deleted on destruction.
class ScopedLog {
   public:
    ScopedLog() {
        wchar_t dir[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, dir);
        std::wstring base(dir, n);
        wchar_t name[MAX_PATH];
        GetTempFileNameW(base.c_str(), L"aelog", 0, name);
        path_ = name;
        ae::InitFileLog(path_, 1u << 20);
    }
    ~ScopedLog() {
        ae::ShutdownFileLog();
        DeleteFileW(ToExtendedPath(path_).c_str());
    }
    const std::wstring& path() const { return path_; }

   private:
    std::wstring path_;
};

}  // namespace

TEST(Passwords, PasswordNeverWrittenToLogOnSuccess) {
    ScopedLog log;
    {
        ScopedTempDir tmp;
        std::atomic<int> prompts{0};
        auto cb = AlwaysReturn(kPassword, &prompts);
        const auto r = RunRouted("7z-aes__password.7z", tmp.path(), cb);
        ASSERT_EQ(r.status, ExtractStatus::Ok) << r.message.c_str();
    }
    const std::string contents = ReadAllBytes(log.path());
    EXPECT_EQ(contents.find("test1234"), std::string::npos)
        << "the password must never appear in the diagnostic log";
}

TEST(Passwords, PasswordNeverWrittenToLogOnWrongPasswordAbort) {
    ScopedLog log;
    {
        ScopedTempDir tmp;
        std::atomic<int> prompts{0};
        auto cb = AlwaysReturn(kWrongPassword, &prompts);
        const auto r = RunRouted("zip-zipcrypto__password.zip", tmp.path(), cb);
        ASSERT_EQ(r.status, ExtractStatus::NeedPassword);
        EXPECT_EQ(CountFiles(tmp.path()), 0u);
    }
    const std::string contents = ReadAllBytes(log.path());
    EXPECT_EQ(contents.find("nope-not-it"), std::string::npos);
    EXPECT_EQ(contents.find("test1234"), std::string::npos);
}
