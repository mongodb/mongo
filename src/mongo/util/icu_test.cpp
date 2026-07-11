// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/icu.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

struct testCases {
    std::string original;
    std::string normalized;
    bool success;
};

TEST(ICUTest, icuSaslPrep) {
    const testCases tests[] = {
        // U+0065 LATIN SMALL LETTER E + U+0301 COMBINING ACUTE ACCENT
        // U+00E9 LATIN SMALL LETTER E WITH ACUTE
        {"\x65\xCC\x81", "\xC3\xA9", true},

        // Test values from RFC4013 Section 3.
        // #1 SOFT HYPHEN mapped to nothing.
        {"I\xC2\xADX", "IX", true},
        // #2 no transformation
        {"user", "user", true},
        // #3 case preserved, will not match #2
        {"USER", "USER", true},
        // #4 output is NFKC, input in ISO 8859-1
        {"\xC2\xAA", "a", true},
        // #5 output is NFKC, will match #1
        {"\xE2\x85\xA8", "IX", true},
        // #6 Error - prohibited character
        {"\x07", "(invalid)", false},
        // #7 Error - bidirectional check
        {"\xD8\xA7\x31", "(invalid)", false},
    };

    for (const auto& test : tests) {
        auto ret = icuSaslPrep(test.original);
        ASSERT_EQ(ret.isOK(), test.success);
        if (test.success) {
            ASSERT_OK(ret);
            ASSERT_EQ(ret.getValue(), test.normalized);
        } else {
            ASSERT_NOT_OK(ret);
        }
    }
}

TEST(ICUTest, icuCaseFold) {
    // ASCII
    ASSERT_EQ(icuCaseFold("Hello"), "hello");
    ASSERT_EQ(icuCaseFold("hello"), "hello");

    // Non-ASCII
    ASSERT_EQ(icuCaseFold("Æbler"), "æbler");
    ASSERT_EQ(icuCaseFold("æbler"), "æbler");
    ASSERT_EQ(icuCaseFold("Ω"), "ω");
    ASSERT_EQ(icuCaseFold("Straße"), "strasse");
    ASSERT_EQ(icuCaseFold("Strasse"), "strasse");
    ASSERT_EQ(icuCaseFold("STRAẞE"), "strasse");

    // Empty string
    ASSERT_EQ(icuCaseFold(""), "");
}

}  // namespace
}  // namespace mongo
