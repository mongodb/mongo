
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/icu.h"

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

    for (const auto test : tests) {
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

}  // namespace
}  // namespace mongo
