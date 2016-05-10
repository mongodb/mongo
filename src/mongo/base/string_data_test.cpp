/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include <algorithm>
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::string;

TEST(Construction, Empty) {
    StringData strData;
    ASSERT_EQUALS(strData.size(), 0U);
    ASSERT_TRUE(strData.rawData() == NULL);
}

TEST(Construction, FromStdString) {
    std::string base("aaa");
    StringData strData(base);
    ASSERT_EQUALS(strData.size(), base.size());
    ASSERT_EQUALS(strData.toString(), base);
}

TEST(Construction, FromCString) {
    std::string base("aaa");
    StringData strData(base.c_str());
    ASSERT_EQUALS(strData.size(), base.size());
    ASSERT_EQUALS(strData.toString(), base);
}

TEST(Construction, FromNullCString) {
    char* c = NULL;
    StringData strData(c);
    ASSERT_EQUALS(strData.size(), 0U);
    ASSERT_TRUE(strData.rawData() == NULL);
}

TEST(Construction, FromUserDefinedLiteral) {
    const auto strData = "cc\0c"_sd;
    ASSERT_EQUALS(strData.size(), 4U);
    ASSERT_EQUALS(strData.toString(), string("cc\0c", 4));
}

TEST(Construction, FromUserDefinedRawLiteral) {
    const auto strData = R"("")"_sd;
    ASSERT_EQUALS(strData.size(), 2U);
    ASSERT_EQUALS(strData.toString(), string("\"\"", 2));
}

TEST(Construction, FromEmptyUserDefinedLiteral) {
    const auto strData = ""_sd;
    ASSERT_EQUALS(strData.size(), 0U);
    ASSERT_EQUALS(strData.toString(), string(""));
}

TEST(Comparison, BothEmpty) {
    StringData empty("");
    ASSERT_TRUE(empty == empty);
    ASSERT_FALSE(empty != empty);
    ASSERT_FALSE(empty > empty);
    ASSERT_TRUE(empty >= empty);
    ASSERT_FALSE(empty < empty);
    ASSERT_TRUE(empty <= empty);
}

TEST(Comparison, BothNonEmptyOnSize) {
    StringData a("a");
    StringData aa("aa");
    ASSERT_FALSE(a == aa);
    ASSERT_TRUE(a != aa);
    ASSERT_FALSE(a > aa);
    ASSERT_FALSE(a >= aa);
    ASSERT_TRUE(a >= a);
    ASSERT_TRUE(a < aa);
    ASSERT_TRUE(a <= aa);
    ASSERT_TRUE(a <= a);
}

TEST(Comparison, BothNonEmptyOnContent) {
    StringData a("a");
    StringData b("b");
    ASSERT_FALSE(a == b);
    ASSERT_TRUE(a != b);
    ASSERT_FALSE(a > b);
    ASSERT_FALSE(a >= b);
    ASSERT_TRUE(a < b);
    ASSERT_TRUE(a <= b);
}

TEST(Comparison, MixedEmptyAndNot) {
    StringData empty("");
    StringData a("a");
    ASSERT_FALSE(a == empty);
    ASSERT_TRUE(a != empty);
    ASSERT_TRUE(a > empty);
    ASSERT_TRUE(a >= empty);
    ASSERT_FALSE(a < empty);
    ASSERT_FALSE(a <= empty);
}

TEST(Find, Char1) {
    ASSERT_EQUALS(string::npos, StringData("foo").find('a'));
    ASSERT_EQUALS(0U, StringData("foo").find('f'));
    ASSERT_EQUALS(1U, StringData("foo").find('o'));
}

TEST(Find, Str1) {
    ASSERT_EQUALS(string::npos, StringData("foo").find("asdsadasda"));
    ASSERT_EQUALS(string::npos, StringData("foo").find("a"));
    ASSERT_EQUALS(string::npos, StringData("foo").find("food"));
    ASSERT_EQUALS(string::npos, StringData("foo").find("ooo"));

    ASSERT_EQUALS(0U, StringData("foo").find("f"));
    ASSERT_EQUALS(0U, StringData("foo").find("fo"));
    ASSERT_EQUALS(0U, StringData("foo").find("foo"));
    ASSERT_EQUALS(1U, StringData("foo").find("o"));
    ASSERT_EQUALS(1U, StringData("foo").find("oo"));

    ASSERT_EQUALS(string("foo").find(""), StringData("foo").find(""));
}

// Helper function for Test(Hasher, Str1)
template <int SizeofSizeT>
void SDHasher_check(void);

template <>
void SDHasher_check<4>(void) {
    ASSERT_EQUALS(StringData::Hasher()(""), static_cast<size_t>(0));
    ASSERT_EQUALS(StringData::Hasher()("foo"), static_cast<size_t>(4138058784ULL));
    ASSERT_EQUALS(StringData::Hasher()("pizza"), static_cast<size_t>(3587803311ULL));
    ASSERT_EQUALS(StringData::Hasher()("mongo"), static_cast<size_t>(3724335885ULL));
    ASSERT_EQUALS(StringData::Hasher()("murmur"), static_cast<size_t>(1945310157ULL));
}

template <>
void SDHasher_check<8>(void) {
    ASSERT_EQUALS(StringData::Hasher()(""), static_cast<size_t>(0));
    ASSERT_EQUALS(StringData::Hasher()("foo"), static_cast<size_t>(16316970633193145697ULL));
    ASSERT_EQUALS(StringData::Hasher()("pizza"), static_cast<size_t>(12165495155477134356ULL));
    ASSERT_EQUALS(StringData::Hasher()("mongo"), static_cast<size_t>(2861051452199491487ULL));
    ASSERT_EQUALS(StringData::Hasher()("murmur"), static_cast<size_t>(18237957392784716687ULL));
}

TEST(Hasher, Str1) {
    SDHasher_check<sizeof(size_t)>();
}

TEST(Rfind, Char1) {
    ASSERT_EQUALS(string::npos, StringData("foo").rfind('a'));

    ASSERT_EQUALS(0U, StringData("foo").rfind('f'));
    ASSERT_EQUALS(0U, StringData("foo").rfind('f', 3));
    ASSERT_EQUALS(0U, StringData("foo").rfind('f', 2));
    ASSERT_EQUALS(0U, StringData("foo").rfind('f', 1));
    ASSERT_EQUALS(string::npos, StringData("foo", 0).rfind('f'));

    ASSERT_EQUALS(2U, StringData("foo").rfind('o'));
    ASSERT_EQUALS(2U, StringData("foo", 3).rfind('o'));
    ASSERT_EQUALS(1U, StringData("foo", 2).rfind('o'));
    ASSERT_EQUALS(string::npos, StringData("foo", 1).rfind('o'));
    ASSERT_EQUALS(string::npos, StringData("foo", 0).rfind('o'));
}

// this is to verify we match std::string
void SUBSTR_TEST_HELP(StringData big, StringData small, size_t start, size_t len) {
    ASSERT_EQUALS(small.toString(), big.toString().substr(start, len));
    ASSERT_EQUALS(small, StringData(big).substr(start, len));
}
void SUBSTR_TEST_HELP(StringData big, StringData small, size_t start) {
    ASSERT_EQUALS(small.toString(), big.toString().substr(start));
    ASSERT_EQUALS(small, StringData(big).substr(start));
}

// [12] is number of args to substr
#define SUBSTR_1_TEST_HELP(big, small, start)                                              \
    ASSERT_EQUALS(StringData(small).toString(), StringData(big).toString().substr(start)); \
    ASSERT_EQUALS(StringData(small), StringData(big).substr(start));

#define SUBSTR_2_TEST_HELP(big, small, start, len)                                              \
    ASSERT_EQUALS(StringData(small).toString(), StringData(big).toString().substr(start, len)); \
    ASSERT_EQUALS(StringData(small), StringData(big).substr(start, len));

TEST(Substr, Simple1) {
    SUBSTR_1_TEST_HELP("abcde", "abcde", 0);
    SUBSTR_2_TEST_HELP("abcde", "abcde", 0, 10);
    SUBSTR_2_TEST_HELP("abcde", "abcde", 0, 5);
    SUBSTR_2_TEST_HELP("abcde", "abc", 0, 3);
    SUBSTR_1_TEST_HELP("abcde", "cde", 2);
    SUBSTR_2_TEST_HELP("abcde", "cde", 2, 5);
    SUBSTR_2_TEST_HELP("abcde", "cde", 2, 3);
    SUBSTR_2_TEST_HELP("abcde", "cd", 2, 2);
    SUBSTR_2_TEST_HELP("abcde", "cd", 2, 2);
    SUBSTR_1_TEST_HELP("abcde", "", 5);
    SUBSTR_2_TEST_HELP("abcde", "", 5, 0);
    SUBSTR_2_TEST_HELP("abcde", "", 5, 10);

    // make sure we don't blow past the end of the StringData
    SUBSTR_1_TEST_HELP(StringData("abcdeXXX", 5), "abcde", 0);
    SUBSTR_2_TEST_HELP(StringData("abcdeXXX", 5), "abcde", 0, 10);
    SUBSTR_1_TEST_HELP(StringData("abcdeXXX", 5), "de", 3);
    SUBSTR_2_TEST_HELP(StringData("abcdeXXX", 5), "de", 3, 7);
    SUBSTR_1_TEST_HELP(StringData("abcdeXXX", 5), "", 5);
    SUBSTR_2_TEST_HELP(StringData("abcdeXXX", 5), "", 5, 1);
}

TEST(equalCaseInsensitiveTest, Simple1) {
    ASSERT(StringData("abc").equalCaseInsensitive("abc"));
    ASSERT(StringData("abc").equalCaseInsensitive("ABC"));
    ASSERT(StringData("ABC").equalCaseInsensitive("abc"));
    ASSERT(StringData("ABC").equalCaseInsensitive("ABC"));
    ASSERT(StringData("ABC").equalCaseInsensitive("AbC"));
    ASSERT(!StringData("ABC").equalCaseInsensitive("AbCd"));
    ASSERT(!StringData("ABC").equalCaseInsensitive("AdC"));
}

TEST(StartsWith, Simple) {
    ASSERT(StringData("").startsWith(""));
    ASSERT(!StringData("").startsWith("x"));
    ASSERT(StringData("abcde").startsWith(""));
    ASSERT(StringData("abcde").startsWith("a"));
    ASSERT(StringData("abcde").startsWith("ab"));
    ASSERT(StringData("abcde").startsWith("abc"));
    ASSERT(StringData("abcde").startsWith("abcd"));
    ASSERT(StringData("abcde").startsWith("abcde"));
    ASSERT(!StringData("abcde").startsWith("abcdef"));
    ASSERT(!StringData("abcde").startsWith("abdce"));
    ASSERT(StringData("abcde").startsWith(StringData("abcdeXXXX").substr(0, 4)));
    ASSERT(!StringData("abcde").startsWith(StringData("abdef").substr(0, 4)));
    ASSERT(!StringData("abcde").substr(0, 3).startsWith("abcd"));
}

TEST(EndsWith, Simple) {
    // ASSERT(StringData("").endsWith(""));
    ASSERT(!StringData("").endsWith("x"));
    // ASSERT(StringData("abcde").endsWith(""));
    ASSERT(StringData("abcde").endsWith(StringData("e", 0)));
    ASSERT(StringData("abcde").endsWith("e"));
    ASSERT(StringData("abcde").endsWith("de"));
    ASSERT(StringData("abcde").endsWith("cde"));
    ASSERT(StringData("abcde").endsWith("bcde"));
    ASSERT(StringData("abcde").endsWith("abcde"));
    ASSERT(!StringData("abcde").endsWith("0abcde"));
    ASSERT(!StringData("abcde").endsWith("abdce"));
    ASSERT(StringData("abcde").endsWith(StringData("bcdef").substr(0, 4)));
    ASSERT(!StringData("abcde").endsWith(StringData("bcde", 3)));
    ASSERT(!StringData("abcde").substr(0, 3).endsWith("cde"));
}

TEST(ConstIterator, StdCopy) {
    std::vector<char> chars;
    auto data = "This is some raw data."_sd;

    chars.resize(data.size());
    std::copy(data.begin(), data.end(), chars.begin());

    for (size_t i = 0; i < data.size(); ++i) {
        ASSERT_EQUALS(data[i], chars[i]);
    }
}

TEST(ConstIterator, StdReverseCopy) {
    std::vector<char> chars;
    auto data = "This is some raw data."_sd;

    chars.resize(data.size());
    std::reverse_copy(data.begin(), data.end(), chars.begin());

    const char rawDataExpected[] = ".atad war emos si sihT";

    for (size_t i = 0; i < data.size(); ++i) {
        ASSERT_EQUALS(rawDataExpected[i], chars[i]);
    }
}

TEST(ConstIterator, StdReplaceCopy) {
    std::vector<char> chars;
    auto data = "This is some raw data."_sd;

    chars.resize(data.size());
    std::replace_copy(data.begin(), data.end(), chars.begin(), ' ', '_');

    const char rawDataExpected[] = "This_is_some_raw_data.";

    for (size_t i = 0; i < data.size(); ++i) {
        ASSERT_EQUALS(rawDataExpected[i], chars[i]);
    }
}

}  // namespace mongo
