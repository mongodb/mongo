#include "bazel/windows_test_launcher/windows_test_launcher.h"

#include <gtest/gtest.h>

namespace mongo::windows_test_launcher {
TEST(WindowsTestLauncher, QuotesArgumentsForWindowsCRT) {
    EXPECT_EQ(quoteArgument(L""), L"\"\"");
    EXPECT_EQ(quoteArgument(L"with spaces"), L"\"with spaces\"");
    EXPECT_EQ(quoteArgument(L"embedded\"quote"), L"\"embedded\\\"quote\"");
    EXPECT_EQ(quoteArgument(L"trailing slash\\"), L"\"trailing slash\\\\\"");
    EXPECT_EQ(quoteArgument(L"slash\\\"quote"), L"\"slash\\\\\\\"quote\"");
    EXPECT_EQ(quoteArgument(L"%PATH%&|<>^!"), L"\"%PATH%&|<>^!\"");
}
}  // namespace mongo::windows_test_launcher
