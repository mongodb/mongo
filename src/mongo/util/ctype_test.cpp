// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include <cstddef>
#include <string>

#include <fmt/format.h>
// IWYU pragma: no_include <ctype.h>

#include "mongo/base/static_assert.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/ctype.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo::ctype {
namespace {

TEST(Ctype, MatchesCxxStdlib) {
    for (size_t i = 0; i < 256; ++i) {
        char c = i;
        unsigned char uc = i;
        const std::string msg = fmt::format(" i={:02x}", i);
        ASSERT_EQ(isAlnum(c), (bool)std::isalnum(uc)) << msg;
        ASSERT_EQ(isAlpha(c), (bool)std::isalpha(uc)) << msg;
        ASSERT_EQ(isLower(c), (bool)std::islower(uc)) << msg;
        ASSERT_EQ(isUpper(c), (bool)std::isupper(uc)) << msg;
        ASSERT_EQ(isDigit(c), (bool)std::isdigit(uc)) << msg;
        ASSERT_EQ(isXdigit(c), (bool)std::isxdigit(uc)) << msg;
        ASSERT_EQ(isCntrl(c), (bool)std::iscntrl(uc)) << msg;
        ASSERT_EQ(isGraph(c), (bool)std::isgraph(uc)) << msg;
        ASSERT_EQ(isSpace(c), (bool)std::isspace(uc)) << msg;
        ASSERT_EQ(isBlank(c), (bool)std::isblank(uc)) << msg;
        ASSERT_EQ(isPrint(c), (bool)std::isprint(uc)) << msg;
        ASSERT_EQ(isPunct(c), (bool)std::ispunct(uc)) << msg;
        ASSERT_EQ(toLower(c), (char)std::tolower(uc)) << msg;
        ASSERT_EQ(toUpper(c), (char)std::toupper(uc)) << msg;
    }
}

TEST(Ctype, MatchesCStdlib) {
    for (size_t i = 0; i < 256; ++i) {
        char c = i;
        unsigned char uc = i;
        const std::string msg = fmt::format(" i={:02x}", i);
        ASSERT_EQ(isAlnum(c), (bool)isalnum(uc)) << msg;
        ASSERT_EQ(isAlpha(c), (bool)isalpha(uc)) << msg;
        ASSERT_EQ(isLower(c), (bool)islower(uc)) << msg;
        ASSERT_EQ(isUpper(c), (bool)isupper(uc)) << msg;
        ASSERT_EQ(isDigit(c), (bool)isdigit(uc)) << msg;
        ASSERT_EQ(isXdigit(c), (bool)isxdigit(uc)) << msg;
        ASSERT_EQ(isCntrl(c), (bool)iscntrl(uc)) << msg;
        ASSERT_EQ(isGraph(c), (bool)isgraph(uc)) << msg;
        ASSERT_EQ(isSpace(c), (bool)isspace(uc)) << msg;
        ASSERT_EQ(isBlank(c), (bool)isblank(uc)) << msg;
        ASSERT_EQ(isPrint(c), (bool)isprint(uc)) << msg;
        ASSERT_EQ(isPunct(c), (bool)ispunct(uc)) << msg;
        ASSERT_EQ(toLower(c), (char)tolower(uc)) << msg;
        ASSERT_EQ(toUpper(c), (char)toupper(uc)) << msg;
    }
}

TEST(Ctype, IsConstexpr) {
    MONGO_STATIC_ASSERT(isAlnum('a'));
    MONGO_STATIC_ASSERT(isAlpha('a'));
    MONGO_STATIC_ASSERT(isLower('a'));
    MONGO_STATIC_ASSERT(!isUpper('a'));
    MONGO_STATIC_ASSERT(!isDigit('a'));
    MONGO_STATIC_ASSERT(isXdigit('a'));
    MONGO_STATIC_ASSERT(!isCntrl('a'));
    MONGO_STATIC_ASSERT(isGraph('a'));
    MONGO_STATIC_ASSERT(!isSpace('a'));
    MONGO_STATIC_ASSERT(!isBlank('a'));
    MONGO_STATIC_ASSERT(isPrint('a'));
    MONGO_STATIC_ASSERT(!isPunct('a'));
    MONGO_STATIC_ASSERT(toLower('a') == 'a');
    MONGO_STATIC_ASSERT(toUpper('a') == 'A');
}

}  // namespace
}  // namespace mongo::ctype
