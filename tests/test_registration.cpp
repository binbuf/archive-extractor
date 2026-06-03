#include "archive_core/registration.h"

#include "archive_core/paths.h"

#include <gtest/gtest.h>

#include <algorithm>

using ae::BuildRegistryPlan;
using ae::HasSupportedExtension;
using ae::RegEntry;
using ae::RegistrationExtensions;
using ae::RegistryPlan;
using ae::RegScope;

namespace {

// Find a single entry by (scope, subKey, valueName); nullptr if absent.
const RegEntry* Find(const RegistryPlan& plan, RegScope scope,
                     const std::wstring& subKey, const std::wstring& valueName) {
    for (const auto& e : plan.entries) {
        if (e.scope == scope && e.subKey == subKey && e.valueName == valueName)
            return &e;
    }
    return nullptr;
}

bool HasDeleteTree(const RegistryPlan& plan, RegScope scope,
                   const std::wstring& subKey) {
    return std::any_of(plan.keysToDelete.begin(), plan.keysToDelete.end(),
                       [&](const auto& p) {
                           return p.first == scope && p.second == subKey;
                       });
}

bool HasDeleteValue(const RegistryPlan& plan, RegScope scope,
                    const std::wstring& subKey, const std::wstring& valueName) {
    return std::any_of(plan.valuesToDelete.begin(), plan.valuesToDelete.end(),
                       [&](const ae::RegValueRef& v) {
                           return v.scope == scope && v.subKey == subKey &&
                                  v.valueName == valueName;
                       });
}

// Narrow an ASCII-only extension token for gtest failure messages (the values
// are all ASCII like ".zip", so a byte cast is lossless and warning-clean).
std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(static_cast<char>(c & 0x7F));
    return s;
}

}  // namespace

TEST(Registration, ExtensionListMatchesSpec) {
    // The spec's exact registration set (with leading dots), order-independent.
    const std::vector<std::wstring> expected = {
        L".zip", L".7z",  L".zst", L".rar", L".xz",  L".gz", L".lz4",
        L".tar", L".bz2", L".br",  L".tgz", L".tbz2", L".txz"};
    auto got = RegistrationExtensions();
    std::sort(got.begin(), got.end());
    auto exp = expected;
    std::sort(exp.begin(), exp.end());
    EXPECT_EQ(got, exp);
}

TEST(Registration, EveryRegisteredExtensionIsAlsoDetected) {
    // Reconciliation guarantee: anything we advertise to the shell must also be
    // accepted by the extraction gate, or a double-click would bounce.
    for (const auto& ext : RegistrationExtensions()) {
        EXPECT_TRUE(HasSupportedExtension(L"sample" + ext))
            << "registered but not detected: " << Narrow(ext);
    }
}

TEST(Registration, ProgIdKeysAndCommand) {
    const RegistryPlan plan = BuildRegistryPlan(L"C:\\Program Files\\AE\\ArchiveExtractor.exe");

    const RegEntry* progId = Find(plan, RegScope::Classes,
                                  L"ArchiveExtractor.Archive.1", L"");
    ASSERT_NE(progId, nullptr);
    EXPECT_EQ(progId->valueData, L"Compressed Archive");

    const RegEntry* icon = Find(plan, RegScope::Classes,
                                L"ArchiveExtractor.Archive.1\\DefaultIcon", L"");
    ASSERT_NE(icon, nullptr);
    EXPECT_EQ(icon->valueData, L"C:\\Program Files\\AE\\ArchiveExtractor.exe,0");

    const RegEntry* cmd =
        Find(plan, RegScope::Classes,
             L"ArchiveExtractor.Archive.1\\shell\\open\\command", L"");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->valueData,
              L"\"C:\\Program Files\\AE\\ArchiveExtractor.exe\" \"%1\"");
}

TEST(Registration, PerExtensionOpenWithProgidsAndCapabilities) {
    const RegistryPlan plan = BuildRegistryPlan(L"X:\\AE\\ArchiveExtractor.exe");
    for (const auto& ext : RegistrationExtensions()) {
        // OpenWithProgids advertises the ProgID as a NAMED VALUE (empty data)
        // under the .<ext>\OpenWithProgids key — not as a subkey.
        const RegEntry* owp =
            Find(plan, RegScope::Classes, ext + L"\\OpenWithProgids",
                 L"ArchiveExtractor.Archive.1");
        ASSERT_NE(owp, nullptr) << Narrow(ext);
        EXPECT_EQ(owp->valueData, L"");

        // Capabilities\FileAssociations\.<ext> -> ProgId
        const RegEntry* fa =
            Find(plan, RegScope::Software,
                 L"ArchiveExtractor\\Capabilities\\FileAssociations", ext);
        ASSERT_NE(fa, nullptr) << Narrow(ext);
        EXPECT_EQ(fa->valueData, L"ArchiveExtractor.Archive.1");
    }
}

TEST(Registration, CapabilitiesAndRegisteredApplication) {
    const RegistryPlan plan = BuildRegistryPlan(L"X:\\AE\\ArchiveExtractor.exe");

    const RegEntry* appName = Find(plan, RegScope::Software,
                                   L"ArchiveExtractor\\Capabilities",
                                   L"ApplicationName");
    ASSERT_NE(appName, nullptr);
    EXPECT_EQ(appName->valueData, L"Archive Extractor");

    const RegEntry* appDesc = Find(plan, RegScope::Software,
                                   L"ArchiveExtractor\\Capabilities",
                                   L"ApplicationDescription");
    ASSERT_NE(appDesc, nullptr);
    EXPECT_FALSE(appDesc->valueData.empty());

    const RegEntry* regApp = Find(plan, RegScope::Software,
                                  L"RegisteredApplications", L"Archive Extractor");
    ASSERT_NE(regApp, nullptr);
    EXPECT_EQ(regApp->valueData, L"Software\\ArchiveExtractor\\Capabilities");
}

TEST(Registration, UnregisterPlanRemovesEverythingItWrote) {
    const RegistryPlan plan = BuildRegistryPlan(L"X:\\AE\\ArchiveExtractor.exe");
    // The ProgID tree and the whole capabilities tree are removed.
    EXPECT_TRUE(HasDeleteTree(plan, RegScope::Classes,
                              L"ArchiveExtractor.Archive.1"));
    EXPECT_TRUE(HasDeleteTree(plan, RegScope::Software, L"ArchiveExtractor"));
    // Our RegisteredApplications value is removed (the shared key survives).
    EXPECT_TRUE(HasDeleteValue(plan, RegScope::Software,
                               L"RegisteredApplications", L"Archive Extractor"));
    // Each per-extension OpenWithProgids VALUE is removed, but neither the
    // OpenWithProgids key nor the bare .<ext> key (other apps may share them).
    for (const auto& ext : RegistrationExtensions()) {
        EXPECT_TRUE(HasDeleteValue(plan, RegScope::Classes,
                                   ext + L"\\OpenWithProgids",
                                   L"ArchiveExtractor.Archive.1"))
            << Narrow(ext);
        EXPECT_FALSE(HasDeleteTree(plan, RegScope::Classes,
                                   ext + L"\\OpenWithProgids"))
            << Narrow(ext);
        EXPECT_FALSE(HasDeleteTree(plan, RegScope::Classes, ext))
            << "must not delete the bare extension key: " << Narrow(ext);
    }
}
