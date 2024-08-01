/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <fmt/format.h>

#include <boost/optional/optional.hpp>

#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/ctype.h"
#include "mongo/util/hex.h"
#include "mongo/util/str.h"
#include "mongo/util/str_escape.h"

namespace mongo {

using std::string;
using namespace fmt::literals;

void assertCmp(int expected, StringData s1, StringData s2, bool lexOnly = false) {
    str::LexNumCmp cmp(lexOnly);
    ASSERT_EQUALS(expected, cmp.cmp(s1, s2, lexOnly));
    ASSERT_EQUALS(expected, cmp.cmp(s1, s2));
    ASSERT_EQUALS(expected < 0, cmp(s1, s2));
}

TEST(StringEscapeTest, ValidUTF8) {
    auto valid = [](const std::string& s) {
        ASSERT(str::validUTF8(StringData(s)));
    };

    auto notValid = [](const std::string& s) {
        ASSERT(!str::validUTF8(StringData(s)));
    };

    valid("A");
    valid("\xC2\xA2");          // Cent: ¢
    valid("\xE2\x82\xAC");      // Euro: €
    valid("\xF0\x9D\x90\x80");  // Blackboard A: 𝐀
    valid("こんにちは");
    valid("😊");
    valid("");

    // Abrupt end
    notValid("\xC2");
    notValid("\xE2\x82");
    notValid("\xF0\x9D\x90");
    notValid("\xC2 ");
    notValid("\xE2\x82 ");
    notValid("\xF0\x9D\x90 ");

    // Too long
    notValid("\xF8\x80\x80\x80\x80");
    notValid("\xFC\x80\x80\x80\x80\x80");
    notValid("\xFE\x80\x80\x80\x80\x80\x80");
    notValid("\xFF\x80\x80\x80\x80\x80\x80\x80");

    notValid("\xF5\x80\x80\x80");  // U+140000 > U+10FFFF
    notValid("\x80");              // Can't start with continuation byte.
    notValid("\xC0\x80");          // 2-byte version of ASCII NUL
    notValid("\xC1\x80");          // 2-byte version of ASCII NUL
    notValid("\xC3\x28");  // First byte indicates 2-byte sequence, but second byte is not in form
                           // 10xxxxxx
    notValid("\xE2\x28\xA1");  // first byte indicates 3-byte sequence, but second byte is not in
                               // form 10xxxxxx
    notValid("\xDE\xA0\x80");  // Surrogate pairs are not valid for UTF-8 (high surrogate)
    notValid("\xF0\x9D\xDC\x80");  // Surrogate pairs are not valid for UTF-8 (low surrogate)
}

}  // namespace mongo
