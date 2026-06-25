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


#include "mongo/base/string_data_comparator.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

namespace mongo {
using namespace std::literals::string_view_literals;
namespace {


TEST(Construction, Empty) {
    std::string_view strData;
    ASSERT_EQUALS(strData.size(), 0U);
    ASSERT_TRUE(strData.data() == nullptr);
}

TEST(Construction, FromStdString) {
    std::string base("aaa");
    std::string_view strData(base);
    ASSERT_EQUALS(strData.size(), base.size());
    ASSERT_EQUALS(std::string{strData}, base);
}

TEST(Construction, FromCString) {
    std::string base("aaa");
    std::string_view strData(base.c_str());
    ASSERT_EQUALS(strData.size(), base.size());
    ASSERT_EQUALS(std::string{strData}, base);
}

TEST(Construction, FromUserDefinedLiteral) {
    const auto strData = "cc\0c"sv;
    ASSERT_EQUALS(strData.size(), 4U);
    ASSERT_EQUALS(std::string{strData}, std::string("cc\0c", 4));
}

TEST(Construction, FromUserDefinedRawLiteral) {
    const auto strData = R"("")"sv;
    ASSERT_EQUALS(strData.size(), 2U);
    ASSERT_EQUALS(std::string{strData}, std::string("\"\"", 2));
}

TEST(Construction, FromEmptyUserDefinedLiteral) {
    const auto strData = ""sv;
    ASSERT_EQUALS(strData.size(), 0U);
    ASSERT_EQUALS(std::string{strData}, std::string(""));
}

// Try some constexpr initializations
TEST(Construction, Constexpr) {
    constexpr std::string_view lit = "1234567"sv;
    ASSERT_EQUALS(lit, "1234567"sv);
    constexpr std::string_view sub = lit.substr(3, 2);
    ASSERT_EQUALS(sub, "45"sv);
#if MONGO_STRING_DATA_CXX20
    constexpr std::string_view range(lit.begin() + 1, lit.end() - 1);
    ASSERT_EQUALS(range, "23456"sv);
#endif
    constexpr char c = lit[1];
    ASSERT_EQUALS(c, '2');
    constexpr std::string_view nully{nullptr, 0};
    ASSERT_EQUALS(nully, ""sv);
    constexpr std::string_view ptr{lit.data() + 1, 3};
    ASSERT_EQUALS(ptr, "234"sv);
}

TEST(Comparison, BothEmpty) {
    std::string_view empty("");
    ASSERT_TRUE(empty == empty);
    ASSERT_FALSE(empty != empty);
    ASSERT_FALSE(empty > empty);
    ASSERT_TRUE(empty >= empty);
    ASSERT_FALSE(empty < empty);
    ASSERT_TRUE(empty <= empty);

    static_assert(""sv.compare(""sv) == 0);
}

TEST(Comparison, BothNonEmptyOnSize) {
    std::string_view a("a");
    std::string_view aa("aa");
    ASSERT_FALSE(a == aa);
    ASSERT_TRUE(a != aa);
    ASSERT_FALSE(a > aa);
    ASSERT_FALSE(a >= aa);
    ASSERT_TRUE(a >= a);
    ASSERT_TRUE(a < aa);
    ASSERT_TRUE(a <= aa);
    ASSERT_TRUE(a <= a);

    static_assert("a"sv.compare("aa"sv) < 0);
}

TEST(Comparison, BothNonEmptyOnContent) {
    std::string_view a("a");
    std::string_view b("b");
    ASSERT_FALSE(a == b);
    ASSERT_TRUE(a != b);
    ASSERT_FALSE(a > b);
    ASSERT_FALSE(a >= b);
    ASSERT_TRUE(a < b);
    ASSERT_TRUE(a <= b);

    static_assert("a"sv.compare("b"sv) < 0);
}

TEST(Comparison, MixedEmptyAndNot) {
    std::string_view empty("");
    std::string_view a("a");
    ASSERT_FALSE(a == empty);
    ASSERT_TRUE(a != empty);
    ASSERT_TRUE(a > empty);
    ASSERT_TRUE(a >= empty);
    ASSERT_FALSE(a < empty);
    ASSERT_FALSE(a <= empty);

    static_assert(""sv.compare("a"sv) < 0);
}

TEST(Find, Char1) {
    ASSERT_EQUALS(std::string::npos, std::string_view("foo").find('a'));
    ASSERT_EQUALS(0U, std::string_view("foo").find('f'));
    ASSERT_EQUALS(1U, std::string_view("foo").find('o'));

    using namespace std::literals;
    const std::string haystacks[]{"foo", "f", "", "\0"s, "f\0"s, "\0f"s, "ffoo", "afoo"};
    const char needles[]{'a', 'f', 'o', '\0'};
    for (const auto& s : haystacks) {
        for (const auto& ch : needles) {
            // Try all possibly-relevent `pos` arguments.
            for (size_t pos = 0; pos < s.size() + 2; ++pos) {
                // All expectations should be consistent with std::string::find.
                auto withStdString = s.find(ch, pos);
                auto withStringData = std::string_view{s}.find(ch, pos);
                ASSERT_EQUALS(withStdString, withStringData)
                    << fmt::format(R"(s:'{}', ch:'{}', pos:{})", s, std::string_view{&ch, 1}, pos);
            }
        }
    }
}

TEST(Find, Str1) {
    ASSERT_EQUALS(std::string::npos, std::string_view("foo").find("asdsadasda"));
    ASSERT_EQUALS(std::string::npos, std::string_view("foo").find("a"));
    ASSERT_EQUALS(std::string::npos, std::string_view("foo").find("food"));
    ASSERT_EQUALS(std::string::npos, std::string_view("foo").find("ooo"));

    ASSERT_EQUALS(0U, std::string_view("foo").find("f"));
    ASSERT_EQUALS(0U, std::string_view("foo").find("fo"));
    ASSERT_EQUALS(0U, std::string_view("foo").find("foo"));
    ASSERT_EQUALS(1U, std::string_view("foo").find("o"));
    ASSERT_EQUALS(1U, std::string_view("foo").find("oo"));

    ASSERT_EQUALS(std::string("foo").find(""), std::string_view("foo").find(""));

    using namespace std::literals;
    const std::string haystacks[]{"", "x", "foo", "fffoo", "\0"s};
    const std::string needles[]{
        "", "x", "asdsadasda", "a", "f", "fo", "foo", "food", "o", "oo", "ooo", "\0"s};
    for (const auto& s : haystacks) {
        for (const auto& sub : needles) {
            // Try all possibly-relevent `pos` arguments.
            for (size_t pos = 0; pos < std::max(s.size(), sub.size()) + 2; ++pos) {
                // All expectations should be consistent with std::string::find.
                auto withStdString = s.find(sub, pos);
                auto withStringData = std::string_view{s}.find(std::string_view{sub}, pos);
                ASSERT_EQUALS(withStdString, withStringData)
                    << fmt::format(R"(s:'{}', sub:'{}', pos:{})", s, sub, pos);
            }
        }
    }
}

TEST(Hasher, Str1) {
    static constexpr size_t sizeofSizeT = sizeof(size_t);
    struct Spec {
        std::string_view str;
        uint32_t h4;
        uint64_t h8;
    };
    static constexpr auto specs = std::to_array<Spec>({
        {""sv, 0, 0},
        {"foo"sv, 0xf6a5c420, 0xe271865701f54561},
        {"pizza"sv, 0xd5d988af, 0xa8d485636af33c14},
        {"mongo"sv, 0xddfcdb0d, 0x27b47f232477579f},
        {"murmur"sv, 0x73f313cd, 0xfd1a3d9eb1a4738f},
    });
    auto tryHash = [](std::string_view str) {
        size_t h = 0;
        simpleStringDataComparator.hash_combine(h, str);
        return h;
    };
    if constexpr (sizeofSizeT == 4) {
        for (auto&& s : specs)
            ASSERT_EQUALS(tryHash(s.str), s.h4) << fmt::format("str={}", s.str);
    } else if constexpr (sizeofSizeT == 8) {
        for (auto&& s : specs)
            ASSERT_EQUALS(tryHash(s.str), s.h8) << fmt::format("str={}", s.str);
    } else {
        FAIL("sizeT weird size") << fmt::format("sizeof(size_t) == {}", sizeofSizeT);
    }
}

TEST(Rfind, Char1) {
    ASSERT_EQUALS(std::string::npos, std::string_view("foo").rfind('a'));

    ASSERT_EQUALS(0U, std::string_view("foo").rfind('f'));
    ASSERT_EQUALS(0U, std::string_view("foo").rfind('f', 3));
    ASSERT_EQUALS(0U, std::string_view("foo").rfind('f', 2));
    ASSERT_EQUALS(0U, std::string_view("foo").rfind('f', 1));
    ASSERT_EQUALS(std::string::npos, std::string_view("foo", 0).rfind('f'));

    ASSERT_EQUALS(2U, std::string_view("foo").rfind('o'));
    ASSERT_EQUALS(2U, std::string_view("foo", 3).rfind('o'));
    ASSERT_EQUALS(1U, std::string_view("foo", 2).rfind('o'));
    ASSERT_EQUALS(std::string::npos, std::string_view("foo", 1).rfind('o'));
    ASSERT_EQUALS(std::string::npos, std::string_view("foo", 0).rfind('o'));

    using namespace std::literals;
    const std::string haystacks[]{"", "x", "foo", "fffoo", "oof", "\0"s};
    const char needles[]{'f', 'o', '\0'};
    for (const auto& s : haystacks) {
        for (const auto& ch : needles) {
            auto validate = [&](size_t pos) {
                // All expectations should be consistent with std::string::rfind.
                auto withStdString = s.rfind(ch, pos);
                auto withStringData = std::string_view{s}.rfind(ch, pos);
                ASSERT_EQUALS(withStdString, withStringData)
                    << fmt::format(R"(s:'{}', ch:'{}', pos:{})", s, std::string_view{&ch, 1}, pos);
            };
            // Try all possibly-relevent `pos` arguments.
            for (size_t pos = 0; pos < s.size() + 2; ++pos)
                validate(pos);
            validate(std::string::npos);
        }
    }
}

// this is to verify we match std::string
void SUBSTR_TEST_HELP(std::string_view big, std::string_view small, size_t start, size_t len) {
    ASSERT_EQUALS(std::string{small}, std::string{big}.substr(start, len));
    ASSERT_EQUALS(small, std::string_view(big).substr(start, len));
}
void SUBSTR_TEST_HELP(std::string_view big, std::string_view small, size_t start) {
    ASSERT_EQUALS(std::string{small}, std::string{big}.substr(start));
    ASSERT_EQUALS(small, std::string_view(big).substr(start));
}

// [12] is number of args to substr
#define SUBSTR_1_TEST_HELP(big, small, start)                        \
    ASSERT_EQUALS(std::string{std::string_view(small)},              \
                  std::string{std::string_view(big)}.substr(start)); \
    ASSERT_EQUALS(std::string_view(small), std::string_view(big).substr(start));

#define SUBSTR_2_TEST_HELP(big, small, start, len)                        \
    ASSERT_EQUALS(std::string{std::string_view(small)},                   \
                  std::string{std::string_view(big)}.substr(start, len)); \
    ASSERT_EQUALS(std::string_view(small), std::string_view(big).substr(start, len));

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

    // make sure we don't blow past the end of the std::string_view
    SUBSTR_1_TEST_HELP(std::string_view("abcdeXXX", 5), "abcde", 0);
    SUBSTR_2_TEST_HELP(std::string_view("abcdeXXX", 5), "abcde", 0, 10);
    SUBSTR_1_TEST_HELP(std::string_view("abcdeXXX", 5), "de", 3);
    SUBSTR_2_TEST_HELP(std::string_view("abcdeXXX", 5), "de", 3, 7);
    SUBSTR_1_TEST_HELP(std::string_view("abcdeXXX", 5), "", 5);
    SUBSTR_2_TEST_HELP(std::string_view("abcdeXXX", 5), "", 5, 1);
}

TEST(StartsWith, Simple) {
    ASSERT(std::string_view("").starts_with(""));
    ASSERT(!std::string_view("").starts_with("x"));
    ASSERT(std::string_view("abcde").starts_with(""));
    ASSERT(std::string_view("abcde").starts_with("a"));
    ASSERT(std::string_view("abcde").starts_with("ab"));
    ASSERT(std::string_view("abcde").starts_with("abc"));
    ASSERT(std::string_view("abcde").starts_with("abcd"));
    ASSERT(std::string_view("abcde").starts_with("abcde"));
    ASSERT(!std::string_view("abcde").starts_with("abcdef"));
    ASSERT(!std::string_view("abcde").starts_with("abdce"));
    ASSERT(std::string_view("abcde").starts_with(std::string_view("abcdeXXXX").substr(0, 4)));
    ASSERT(!std::string_view("abcde").starts_with(std::string_view("abdef").substr(0, 4)));
    ASSERT(!std::string_view("abcde").substr(0, 3).starts_with("abcd"));
}

TEST(EndsWith, Simple) {
    // ASSERT(std::string_view("").endsWith(""));
    ASSERT(!std::string_view("").ends_with("x"));
    // ASSERT(std::string_view("abcde").endsWith(""));
    ASSERT(std::string_view("abcde").ends_with(std::string_view("e", 0)));
    ASSERT(std::string_view("abcde").ends_with("e"));
    ASSERT(std::string_view("abcde").ends_with("de"));
    ASSERT(std::string_view("abcde").ends_with("cde"));
    ASSERT(std::string_view("abcde").ends_with("bcde"));
    ASSERT(std::string_view("abcde").ends_with("abcde"));
    ASSERT(!std::string_view("abcde").ends_with("0abcde"));
    ASSERT(!std::string_view("abcde").ends_with("abdce"));
    ASSERT(std::string_view("abcde").ends_with(std::string_view("bcdef").substr(0, 4)));
    ASSERT(!std::string_view("abcde").ends_with(std::string_view("bcde", 3)));
    ASSERT(!std::string_view("abcde").substr(0, 3).ends_with("cde"));
}

TEST(ConstIterator, StdCopy) {
    std::vector<char> chars;
    auto data = "This is some raw data."sv;

    chars.resize(data.size());
    std::copy(data.begin(), data.end(), chars.begin());

    for (size_t i = 0; i < data.size(); ++i) {
        ASSERT_EQUALS(data[i], chars[i]);
    }
}

TEST(ConstIterator, StdReverseCopy) {
    std::vector<char> chars;
    auto data = "This is some raw data."sv;

    chars.resize(data.size());
    std::reverse_copy(data.begin(), data.end(), chars.begin());

    const char rawDataExpected[] = ".atad war emos si sihT";

    for (size_t i = 0; i < data.size(); ++i) {
        ASSERT_EQUALS(rawDataExpected[i], chars[i]);
    }
}

TEST(ConstIterator, StdReplaceCopy) {
    std::vector<char> chars;
    auto data = "This is some raw data."sv;

    chars.resize(data.size());
    std::replace_copy(data.begin(), data.end(), chars.begin(), ' ', '_');

    const char rawDataExpected[] = "This_is_some_raw_data.";

    for (size_t i = 0; i < data.size(); ++i) {
        ASSERT_EQUALS(rawDataExpected[i], chars[i]);
    }
}

TEST(StringDataFmt, Fmt) {
    ASSERT_EQUALS(fmt::format("-{}-", "abc"sv), "-abc-");
}

TEST(Ostream, StringDataMatchesStdString) {
    const std::string s = "xyz";
    struct TestCase {
        int line;
        std::function<void(std::ostream&)> manip;
    };
    const TestCase testCases[] = {
        {__LINE__,
         [](std::ostream& os) {
         }},
        {__LINE__,
         [](std::ostream& os) {
             os << std::setw(5);
         }},
        {__LINE__,
         [](std::ostream& os) {
             os << std::left << std::setw(5);
         }},
        {__LINE__,
         [](std::ostream& os) {
             os << std::right << std::setw(5);
         }},
        {__LINE__,
         [](std::ostream& os) {
             os << std::setfill('.') << std::left << std::setw(5);
         }},
        {__LINE__,
         [](std::ostream& os) {
             os << std::setfill('.') << std::right << std::setw(5);
         }},
    };
    for (const auto& testCase : testCases) {
        const std::string location = std::string(" at line:") + std::to_string(testCase.line);
        struct Experiment {
            Experiment(std::function<void(std::ostream&)> f) : putter(f) {}
            std::function<void(std::ostream&)> putter;
            std::ostringstream os;
        };
        Experiment expected{[&](std::ostream& os) {
            os << s;
        }};
        Experiment actual{[&](std::ostream& os) {
            os << std::string_view(s);
        }};
        for (auto& x : {&expected, &actual}) {
            x->os << ">>";
            testCase.manip(x->os);
            x->putter(x->os);
        }
        // ASSERT_EQ(expected.os.str(), actual.os.str()) << location;
        for (auto& x : {&expected, &actual}) {
            x->os << "<<";
        }
        ASSERT_EQ(expected.os.str(), actual.os.str()) << location;
    }
}

TEST(StringDataTest, PlusEq) {
    auto str = std::string("hello ");
    auto& ret = str += "world"sv;
    ASSERT_EQ(str, "hello world");
    ASSERT_EQ(&ret, &str);
}

TEST(StringDataTest, GtestPrintTo) {
    std::string s(256, '\0');
    std::string_view sd{s};
    std::iota(s.begin(), s.end(), '\0');
    for (auto m = s.begin(); m != s.end(); std::rotate(s.begin(), m++, s.end()))
        ASSERT_EQ(testing::PrintToString(sd), testing::PrintToString(sd));
}

}  // namespace
}  // namespace mongo
