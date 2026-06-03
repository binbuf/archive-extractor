#include "archive_core/cli.h"

#include <gtest/gtest.h>

using ae::Mode;
using ae::ParseCommandLine;

TEST(Cli, ExtractModeTakesFirstNonFlagToken) {
    const auto cl = ParseCommandLine({L"C:\\tmp\\a.zip"});
    EXPECT_EQ(cl.mode, Mode::Extract);
    EXPECT_EQ(cl.archivePath, L"C:\\tmp\\a.zip");
}

TEST(Cli, RegisterModeBothSpellings) {
    EXPECT_EQ(ParseCommandLine({L"--register"}).mode, Mode::Register);
    EXPECT_EQ(ParseCommandLine({L"/register"}).mode, Mode::Register);
    EXPECT_EQ(ParseCommandLine({L"--REGISTER"}).mode, Mode::Register);
}

TEST(Cli, UnregisterMode) {
    EXPECT_EQ(ParseCommandLine({L"--unregister"}).mode, Mode::Unregister);
}

TEST(Cli, NoArgsIsNoneNotError) {
    EXPECT_EQ(ParseCommandLine({}).mode, Mode::None);
    EXPECT_TRUE(ParseCommandLine({}).archivePath.empty());
}

TEST(Cli, HelpFlagSetsUsage) {
    EXPECT_TRUE(ParseCommandLine({L"--help"}).showUsage);
    EXPECT_TRUE(ParseCommandLine({L"/?"}).showUsage);
}
