/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/base64.h"

namespace mongo {
namespace {

TEST(Base64Test, transcode) {
    const struct {
        std::string plain;
        std::string encoded;
    } tests[] = {
        {"", ""},
        {"a", "YQ=="},
        {"aa", "YWE="},
        {"aaa", "YWFh"},
        {"aaaa", "YWFhYQ=="},

        {"A", "QQ=="},
        {"AA", "QUE="},
        {"AAA", "QUFB"},
        {"AAAA", "QUFBQQ=="},

        {"The quick brown fox jumped over the lazy dog.",
         "VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wZWQgb3ZlciB0aGUgbGF6eSBkb2cu"},
        {std::string("\0\1\2\3\4\5\6\7", 8), "AAECAwQFBgc="},
        {std::string("\0\277\1\276\2\275", 6), "AL8BvgK9"},
    };

    for (auto const& t : tests) {
        ASSERT_TRUE(base64::validate(t.encoded));

        ASSERT_EQUALS(base64::encode(t.plain), t.encoded);
        ASSERT_EQUALS(base64::decode(t.encoded), t.plain);
    }
}

TEST(Base64Test, parseFail) {
    const struct {
        std::string encoded;
        int code;
    } tests[] = {
        {"BadLength", 10270},
        {"Has Whitespace==", 40537},
        {"Hasbadchar$=", 40537},
        {"Hasbadchar\xFF=", 40537},
        {"Hasbadcahr\t=", 40537},
        {"too=soon", 40538},
    };

    for (auto const& t : tests) {
        ASSERT_FALSE(base64::validate(t.encoded));

        try {
            base64::decode(t.encoded);
            ASSERT_TRUE(false);
        } catch (const AssertionException& e) {
            ASSERT_EQ(e.code(), t.code);
        }
    }
}

}  // namespace
}  // namespace mongo
