/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>
#include <fmt/format.h>

#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/ctype.h"
#include "mongo/util/hex.h"

namespace mongo::ctype {
namespace {

using namespace fmt::literals;

TEST(Ctype, MatchesCxxStdlib) {
    for (size_t i = 0; i < 256; ++i) {
        char c = i;
        unsigned char uc = i;
        const std::string msg = " i={:02x}"_format(i);
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
        const std::string msg = " i={:02x}"_format(i);
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
