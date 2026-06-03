#include "archive_core/paths.h"

#include <gtest/gtest.h>

using ae::HasSupportedExtension;
using ae::ParentDirectory;

TEST(Paths, ParentDirectoryDerivesWorkingDir) {
    EXPECT_EQ(ParentDirectory(L"C:\\Users\\dan\\Downloads\\p.tar.gz"),
              L"C:\\Users\\dan\\Downloads");
}

TEST(Paths, SupportedSingleExtensions) {
    EXPECT_TRUE(HasSupportedExtension(L"a.zip"));
    EXPECT_TRUE(HasSupportedExtension(L"a.7z"));
    EXPECT_TRUE(HasSupportedExtension(L"C:\\x\\y.tar"));
    EXPECT_TRUE(HasSupportedExtension(L"a.BR"));  // case-insensitive
}

TEST(Paths, SupportedCompoundExtensions) {
    EXPECT_TRUE(HasSupportedExtension(L"a.tar.gz"));
    EXPECT_TRUE(HasSupportedExtension(L"a.tar.zst"));
    EXPECT_TRUE(HasSupportedExtension(L"a.tgz"));
}

TEST(Paths, UnsupportedExtensions) {
    EXPECT_FALSE(HasSupportedExtension(L"a.txt"));
    EXPECT_FALSE(HasSupportedExtension(L"noextension"));
    EXPECT_FALSE(HasSupportedExtension(L"trailingdot."));
}
