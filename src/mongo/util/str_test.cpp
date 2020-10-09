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

#include <bitset>
#include <fmt/format.h>

#include "mongo/unittest/unittest.h"

#include "mongo/util/ctype.h"
#include "mongo/util/hex.h"
#include "mongo/util/str.h"

namespace mongo::str {

using namespace fmt::literals;
using std::string;

TEST(StringUtilsTest, Basic) {
    //
    // Basic version comparison tests with different version string types
    //

    // Equal
    ASSERT(versionCmp("1.2.3", "1.2.3") == 0);

    // Basic
    ASSERT(versionCmp("1.2.3", "1.2.4") < 0);
    ASSERT(versionCmp("1.2.3", "1.2.20") < 0);
    ASSERT(versionCmp("1.2.3", "1.20.3") < 0);
    ASSERT(versionCmp("2.2.3", "10.2.3") < 0);

    // Post-fixed
    ASSERT(versionCmp("1.2.3", "1.2.3-") > 0);
    ASSERT(versionCmp("1.2.3", "1.2.3-pre") > 0);
    ASSERT(versionCmp("1.2.3", "1.2.4-") < 0);
    ASSERT(versionCmp("1.2.3-", "1.2.3") < 0);
    ASSERT(versionCmp("1.2.3-pre", "1.2.3") < 0);
}

TEST(StringUtilsTest, Simple1) {
    ASSERT_EQUALS(0, LexNumCmp::cmp("a.b.c", "a.b.c", false));
}

void assertCmp(int expected, StringData s1, StringData s2, bool lexOnly = false) {
    LexNumCmp cmp(lexOnly);
    ASSERT_EQUALS(expected, cmp.cmp(s1, s2, lexOnly));
    ASSERT_EQUALS(expected, cmp.cmp(s1, s2));
    ASSERT_EQUALS(expected < 0, cmp(s1, s2));
}

TEST(StringUtilsTest, Simple2) {
    ASSERT(!ctype::isDigit((char)255));

    assertCmp(0, "a", "a");
    assertCmp(-1, "a", "aa");
    assertCmp(1, "aa", "a");
    assertCmp(-1, "a", "b");
    assertCmp(1, "100", "50");
    assertCmp(-1, "50", "100");
    assertCmp(1, "b", "a");
    assertCmp(0, "aa", "aa");
    assertCmp(-1, "aa", "ab");
    assertCmp(1, "ab", "aa");
    assertCmp(1, "0", "a");
    assertCmp(1, "a0", "aa");
    assertCmp(-1, "a", "0");
    assertCmp(-1, "aa", "a0");
    assertCmp(0, "0", "0");
    assertCmp(0, "10", "10");
    assertCmp(-1, "1", "10");
    assertCmp(1, "10", "1");
    assertCmp(1, "11", "10");
    assertCmp(-1, "10", "11");
    assertCmp(1, "f11f", "f10f");
    assertCmp(-1, "f10f", "f11f");
    assertCmp(-1, "f11f", "f111");
    assertCmp(1, "f111", "f11f");
    assertCmp(-1, "f12f", "f12g");
    assertCmp(1, "f12g", "f12f");
    assertCmp(1, "aa{", "aab");
    assertCmp(-1, "aa{", "aa1");
    assertCmp(-1, "a1{", "a11");
    assertCmp(1, "a1{a", "a1{");
    assertCmp(-1, "a1{", "a1{a");
    assertCmp(1, "21", "11");
    assertCmp(-1, "11", "21");

    assertCmp(-1, "a.0", "a.1");
    assertCmp(-1, "a.0.b", "a.1");

    assertCmp(-1, "b.", "b.|");
    assertCmp(-1, "b.0e", (string("b.") + (char)255).c_str());
    assertCmp(-1, "b.", "b.0e");

    assertCmp(0, "238947219478347782934718234", "238947219478347782934718234");
    assertCmp(0, "000238947219478347782934718234", "238947219478347782934718234");
    assertCmp(1, "000238947219478347782934718235", "238947219478347782934718234");
    assertCmp(-1, "238947219478347782934718234", "238947219478347782934718234.1");
    assertCmp(0, "238", "000238");
    assertCmp(0, "002384", "0002384");
    assertCmp(0, "00002384", "0002384");
    assertCmp(0, "0", "0");
    assertCmp(0, "0000", "0");
    assertCmp(0, "0", "000");
    assertCmp(-1, "0000", "0.0");
    assertCmp(1, "2380", "238");
    assertCmp(1, "2385", "2384");
    assertCmp(1, "2385", "02384");
    assertCmp(1, "2385", "002384");
    assertCmp(-1, "123.234.4567", "00238");
    assertCmp(0, "123.234", "00123.234");
    assertCmp(0, "a.123.b", "a.00123.b");
    assertCmp(1, "a.123.b", "a.b.00123.b");
    assertCmp(-1, "a.00.0", "a.0.1");
    assertCmp(0, "01.003.02", "1.3.2");
    assertCmp(-1, "1.3.2", "10.300.20");
    assertCmp(0, "10.300.20", "000000000000010.0000300.000000020");
    assertCmp(0, "0000a", "0a");
    assertCmp(-1, "a", "0a");
    assertCmp(-1, "000a", "001a");
    assertCmp(0, "010a", "0010a");

    assertCmp(-1, "a0", "a00");
    assertCmp(0, "a.0", "a.00");
    assertCmp(-1, "a.b.c.d0", "a.b.c.d00");
    assertCmp(1, "a.b.c.0.y", "a.b.c.00.x");

    assertCmp(-1, "a", "a-");
    assertCmp(1, "a-", "a");
    assertCmp(0, "a-", "a-");

    assertCmp(-1, "a", "a-c");
    assertCmp(1, "a-c", "a");
    assertCmp(0, "a-c", "a-c");

    assertCmp(1, "a-c.t", "a.t");
    assertCmp(-1, "a.t", "a-c.t");
    assertCmp(0, "a-c.t", "a-c.t");

    assertCmp(1, "ac.t", "a.t");
    assertCmp(-1, "a.t", "ac.t");
    assertCmp(0, "ac.t", "ac.t");
}

TEST(StringUtilsTest, LexOnly) {
    assertCmp(-1, "0", "00", true);
    assertCmp(1, "1", "01", true);
    assertCmp(-1, "1", "11", true);
    assertCmp(1, "2", "11", true);
}

TEST(StringUtilsTest, Substring1) {
    assertCmp(0, "1234", "1234", false);
    assertCmp(0, StringData("1234"), StringData("1234"), false);
    assertCmp(0, StringData("1234", 4), StringData("1234", 4), false);
    assertCmp(-1, StringData("123", 3), StringData("1234", 4), false);


    assertCmp(0, StringData("0001", 3), StringData("0000", 3), false);
}

TEST(StringUtilsTest, UnsignedHex) {
    ASSERT_EQUALS(unsignedHex(0), "0");
    ASSERT_EQUALS(unsignedHex(1), "1");
    ASSERT_EQUALS(unsignedHex(0x1337), "1337");
    ASSERT_EQUALS(unsignedHex(-11111), "FFFFD499");
    ASSERT_EQUALS(unsignedHex(-234987324), "F1FE60C4");
    ASSERT_EQUALS(unsignedHex(std::numeric_limits<int>::min()), "80000000");
    ASSERT_EQUALS(unsignedHex(std::numeric_limits<int>::max()), "7FFFFFFF");
    ASSERT_EQUALS(unsignedHex(std::numeric_limits<long long>::max()), "7FFFFFFFFFFFFFFF");
    ASSERT_EQUALS(unsignedHex(std::numeric_limits<long long>::min()), "8000000000000000");
}

TEST(StringUtilsTest, ZeroPaddedHex) {
    ASSERT_EQUALS(zeroPaddedHex(std::numeric_limits<uint32_t>::max()), "FFFFFFFF");
    ASSERT_EQUALS(zeroPaddedHex(uint32_t{123}), "0000007B");
    ASSERT_EQUALS(zeroPaddedHex(uint8_t{0}), "00");
    ASSERT_EQUALS(zeroPaddedHex(uint16_t{0}), "0000");
    ASSERT_EQUALS(zeroPaddedHex(uint32_t{0}), "00000000");
    ASSERT_EQUALS(zeroPaddedHex(uint64_t{0}), "0000000000000000");
}

TEST(StringUtilsTest, CanParseZero) {
    boost::optional<size_t> result = parseUnsignedBase10Integer("0");
    ASSERT(result && *result == 0);
}

TEST(StringUtilsTest, CanParseDoubleZero) {
    boost::optional<size_t> result = parseUnsignedBase10Integer("00");
    ASSERT(result && *result == 0);
}

TEST(StringUtilsTest, PositivePrefixFailsToParse) {
    boost::optional<size_t> result = parseUnsignedBase10Integer("+0");
    ASSERT(!result);
}

TEST(StringUtilsTest, NegativePrefixFailsToParse) {
    boost::optional<size_t> result = parseUnsignedBase10Integer("-0");
    ASSERT(!result);
}

TEST(StringUtilsTest, CanParseIntValue) {
    boost::optional<size_t> result = parseUnsignedBase10Integer("10");
    ASSERT(result && *result == 10);
}

TEST(StringUtilsTest, CanParseIntValueWithLeadingZeros) {
    boost::optional<size_t> result = parseUnsignedBase10Integer("0010");
    ASSERT(result && *result == 10);
}

TEST(StringUtilsTest, TrailingLetterFailsToParse) {
    boost::optional<size_t> result = parseUnsignedBase10Integer("5a");
    ASSERT(!result);
}

TEST(StringUtilsTest, LeadingLetterFailsToParse) {
    boost::optional<size_t> result = parseUnsignedBase10Integer("a5");
    ASSERT(!result);
}

TEST(StringUtilsTest, LetterWithinNumberFailsToParse) {
    boost::optional<size_t> result = parseUnsignedBase10Integer("5a5");
    ASSERT(!result);
}

TEST(StringUtilsTest, HexStringFailsToParse) {
    boost::optional<size_t> result = parseUnsignedBase10Integer("0xfeed");
    ASSERT(!result);
}

TEST(StringUtilsTest, BinaryStringFailsToParse) {
    boost::optional<size_t> result = parseUnsignedBase10Integer("0b11010010");
    ASSERT(!result);
}

TEST(StringUtilsTest, LeadingWhitespaceFailsToParse) {
    boost::optional<size_t> result = parseUnsignedBase10Integer(" 10");
    ASSERT(!result);
}

TEST(StringUtilsTest, TrailingWhitespaceFailsToParse) {
    boost::optional<size_t> result = parseUnsignedBase10Integer("10 ");
    ASSERT(!result);
}

TEST(StringUtilsTest, WhitespaceWithinNumberFailsToParse) {
    boost::optional<size_t> result = parseUnsignedBase10Integer(" 10");
    ASSERT(!result);
}

TEST(StringUtilsTest, ConvertDoubleToStringWithProperPrecision) {
    ASSERT_EQUALS(std::string("1.9876543219876543"), convertDoubleToString(1.98765432198765432));
    ASSERT_EQUALS(std::string("1.987654321"), convertDoubleToString(1.987654321, 10));
    ASSERT_EQUALS(std::string("1.988"), convertDoubleToString(1.987654321, 4));
    ASSERT_EQUALS(std::string("6e-07"), convertDoubleToString(6E-7, 10));
    ASSERT_EQUALS(std::string("6e-07"), convertDoubleToString(6E-7, 6));
    ASSERT_EQUALS(std::string("0.1000000006"), convertDoubleToString(0.1 + 6E-10, 10));
    ASSERT_EQUALS(std::string("0.1"), convertDoubleToString(0.1 + 6E-8, 6));
}

TEST(StringUtilsTest, UTF8SafeTruncation) {
    // Empty string and ASCII works like normal truncation
    ASSERT_EQUALS(UTF8SafeTruncation(""_sd, 10), ""_sd);
    ASSERT_EQUALS(UTF8SafeTruncation("abcdefg"_sd, 5), "abcde"_sd);

    // Valid 2 Octet sequences, LATIN SMALL LETTER N WITH TILDE
    ASSERT_EQUALS(UTF8SafeTruncation("\u00f1\u00f1\u00f1"_sd, 1), ""_sd);
    ASSERT_EQUALS(UTF8SafeTruncation("\u00f1\u00f1\u00f1"_sd, 4), "\u00f1\u00f1"_sd);
    ASSERT_EQUALS(UTF8SafeTruncation("\u00f1\u00f1\u00f1"_sd, 5), "\u00f1\u00f1"_sd);
    ASSERT_EQUALS(UTF8SafeTruncation("\u00f1\u00f1\u00f1"_sd, 6), "\u00f1\u00f1\u00f1"_sd);

    // Valid 3 Octet sequences, RUNIC LETTER TIWAZ TIR TYR T
    ASSERT_EQUALS(UTF8SafeTruncation("\u16cf\u16cf"_sd, 2), ""_sd);
    ASSERT_EQUALS(UTF8SafeTruncation("\u16cf\u16cf"_sd, 3), "\u16cf"_sd);
    ASSERT_EQUALS(UTF8SafeTruncation("\u16cf\u16cf"_sd, 4), "\u16cf"_sd);
    ASSERT_EQUALS(UTF8SafeTruncation("\u16cf\u16cf"_sd, 5), "\u16cf"_sd);
    ASSERT_EQUALS(UTF8SafeTruncation("\u16cf\u16cf"_sd, 6), "\u16cf\u16cf"_sd);

    // Valid 4 Octet sequences, GOTHIC LETTER MANNA
    ASSERT_EQUALS(UTF8SafeTruncation("\U0001033c\U0001033c"_sd, 4), "\U0001033c"_sd);
    ASSERT_EQUALS(UTF8SafeTruncation("\U0001033c\U0001033c"_sd, 5), "\U0001033c"_sd);
    ASSERT_EQUALS(UTF8SafeTruncation("\U0001033c\U0001033c"_sd, 6), "\U0001033c"_sd);
    ASSERT_EQUALS(UTF8SafeTruncation("\U0001033c\U0001033c"_sd, 7), "\U0001033c"_sd);
    ASSERT_EQUALS(UTF8SafeTruncation("\U0001033c\U0001033c"_sd, 8), "\U0001033c\U0001033c"_sd);
}

TEST(StringUtilsTest, GetCodePointLength) {
    for (int i = 0x0; i < 0x100; ++i) {
        size_t n = 0;
        for (std::bitset<8> bs(i); bs[7 - n]; ++n) {
        }
        if (n == 1)
            continue;  // Avoid the invariant on 0b10xx'xxxx continuation bytes.
        if (n == 0)
            n = 1;  // 7-bit single byte code point.
        ASSERT_EQUALS(getCodePointLength(static_cast<char>(i)), n) << " i:0x{:02x}"_format(i);
    }
}

}  // namespace mongo::str
