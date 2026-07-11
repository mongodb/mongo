// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/str.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/hex.h"
#include "mongo/util/str_escape.h"

#include <algorithm>
#include <bitset>
#include <limits>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo::str {
namespace {
using namespace std::literals::string_view_literals;

using std::string;

TEST(StringUtilsTest, Simple1) {
    ASSERT_EQUALS(0, LexNumCmp::cmp("a.b.c", "a.b.c", false));
}

void assertCmp(int expected, std::string_view s1, std::string_view s2, bool lexOnly = false) {
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
    assertCmp(0, std::string_view("1234"), std::string_view("1234"), false);
    assertCmp(0, std::string_view("1234", 4), std::string_view("1234", 4), false);
    assertCmp(-1, std::string_view("123", 3), std::string_view("1234", 4), false);


    assertCmp(0, std::string_view("0001", 3), std::string_view("0000", 3), false);
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

TEST(StringUtilsTest, EqualCaseInsensitive) {
    ASSERT(str::equalCaseInsensitive(std::string_view("abc"), "abc"));
    ASSERT(str::equalCaseInsensitive(std::string_view("abc"), "ABC"));
    ASSERT(str::equalCaseInsensitive(std::string_view("ABC"), "abc"));
    ASSERT(str::equalCaseInsensitive(std::string_view("ABC"), "ABC"));
    ASSERT(str::equalCaseInsensitive(std::string_view("ABC"), "AbC"));
    ASSERT(!str::equalCaseInsensitive(std::string_view("ABC"), "AbCd"));
    ASSERT(!str::equalCaseInsensitive(std::string_view("ABC"), "AdC"));
}

TEST(StringUtilsTest, UTF8SafeTruncation) {
    // Empty string and ASCII works like normal truncation
    ASSERT_EQUALS(UTF8SafeTruncation(""sv, 10), ""sv);
    ASSERT_EQUALS(UTF8SafeTruncation("abcdefg"sv, 5), "abcde"sv);

    // Valid 2 Octet sequences, LATIN SMALL LETTER N WITH TILDE
    ASSERT_EQUALS(UTF8SafeTruncation("\u00f1\u00f1\u00f1"sv, 1), ""sv);
    ASSERT_EQUALS(UTF8SafeTruncation("\u00f1\u00f1\u00f1"sv, 4), "\u00f1\u00f1"sv);
    ASSERT_EQUALS(UTF8SafeTruncation("\u00f1\u00f1\u00f1"sv, 5), "\u00f1\u00f1"sv);
    ASSERT_EQUALS(UTF8SafeTruncation("\u00f1\u00f1\u00f1"sv, 6), "\u00f1\u00f1\u00f1"sv);

    // Valid 3 Octet sequences, RUNIC LETTER TIWAZ TIR TYR T
    ASSERT_EQUALS(UTF8SafeTruncation("\u16cf\u16cf"sv, 2), ""sv);
    ASSERT_EQUALS(UTF8SafeTruncation("\u16cf\u16cf"sv, 3), "\u16cf"sv);
    ASSERT_EQUALS(UTF8SafeTruncation("\u16cf\u16cf"sv, 4), "\u16cf"sv);
    ASSERT_EQUALS(UTF8SafeTruncation("\u16cf\u16cf"sv, 5), "\u16cf"sv);
    ASSERT_EQUALS(UTF8SafeTruncation("\u16cf\u16cf"sv, 6), "\u16cf\u16cf"sv);

    // Valid 4 Octet sequences, GOTHIC LETTER MANNA
    ASSERT_EQUALS(UTF8SafeTruncation("\U0001033c\U0001033c"sv, 4), "\U0001033c"sv);
    ASSERT_EQUALS(UTF8SafeTruncation("\U0001033c\U0001033c"sv, 5), "\U0001033c"sv);
    ASSERT_EQUALS(UTF8SafeTruncation("\U0001033c\U0001033c"sv, 6), "\U0001033c"sv);
    ASSERT_EQUALS(UTF8SafeTruncation("\U0001033c\U0001033c"sv, 7), "\U0001033c"sv);
    ASSERT_EQUALS(UTF8SafeTruncation("\U0001033c\U0001033c"sv, 8), "\U0001033c\U0001033c"sv);
}

TEST(StringUtilsTest, GetCodePointLength) {
    for (int i = 0x0; i < 0x100; ++i) {
        size_t n = 0;
        for (std::bitset<8> bs(i); n < bs.size() && bs[7 - n]; ++n) {
        }
        if (n == 1)
            continue;  // Avoid the invariant on 0b10xx'xxxx continuation bytes.
        if (n == 0)
            n = 1;  // 7-bit single byte code point.
        ASSERT_EQUALS(getCodePointLength(static_cast<char>(i)), n) << fmt::format(" i:0x{:02x}", i);
    }
}

TEST(StringUtilsTest, UassertNoEmbeddedNulBytes) {
    // These shouldn't throw.
    uassertNoEmbeddedNulBytes({nullptr, 0});
    uassertNoEmbeddedNulBytes(""sv);
    uassertNoEmbeddedNulBytes("hello"sv);
    uassertNoEmbeddedNulBytes("hello\0"sv.substr(0, 5));

    // These should throw.
    ASSERT_THROWS_CODE(uassertNoEmbeddedNulBytes("\0"sv), DBException, 9527900);
    ASSERT_THROWS_CODE(uassertNoEmbeddedNulBytes("\0hello"sv), DBException, 9527900);
    ASSERT_THROWS_CODE(uassertNoEmbeddedNulBytes("hello\0"sv), DBException, 9527900);
    ASSERT_THROWS_CODE(uassertNoEmbeddedNulBytes("hello\0world"sv), DBException, 9527900);
}

TEST(StringUtilsTest, CopyAsCString) {
    char dest[100];  // big enough for anything we would reasonably add here.

    // Print address not contents on failures.
    auto ptr = [](const char* p) {
        return static_cast<const void*>(p);
    };
    auto testValid = [&](std::string_view noNul, int line) {
        // Make sure we write a nul byte. Without this, the test could pass if dest happened to have
        // uninitialized zero bytes.
        std::fill_n(dest, sizeof(dest), 0xff);

        ASSERT_EQ(ptr(copyAsCString(dest, noNul)), ptr(dest + noNul.size() + 1)) << "line:" << line;
        ASSERT_EQ(dest[noNul.size()], '\0') << "line:" << line;
        ASSERT_EQ(std::string_view(dest, noNul.size()), noNul) << "line:" << line;
    };

    // These shouldn't throw.
    testValid({nullptr, 0}, __LINE__);
    testValid(""sv, __LINE__);
    testValid("hello"sv, __LINE__);
    testValid("hello world"sv.substr(0, 5), __LINE__);

    // These should throw.
    ASSERT_THROWS_CODE(copyAsCString(dest, "\0"sv), DBException, 9527900);
    ASSERT_THROWS_CODE(copyAsCString(dest, "\0hello"sv), DBException, 9527900);
    ASSERT_THROWS_CODE(copyAsCString(dest, "hello\0"sv), DBException, 9527900);
    ASSERT_THROWS_CODE(copyAsCString(dest, "hello\0world"sv), DBException, 9527900);
}

/**
 * The Unicode REPLACEMENT_CHARACTER (U+FFFD).
 * https://en.wikipedia.org/wiki/Specials_(Unicode_block)#Replacement_character
 */
const std::string replacementCharacter = u8"\ufffd"_as_char_ptr;

/** Repeat the `s` string, `x` times. */
std::string repeat(std::string_view s, size_t x) {
    std::string result;
    result.reserve(x * s.size());
    auto it = std::back_inserter(result);
    for (size_t i = 0; i < x; ++i)
        it = std::copy(s.begin(), s.end(), it);
    return result;
}

/** Function to convert a Unicode code point to a UTF-8 encoded string */
std::string codePointToUTF8(unsigned int codePoint) {
    std::string result;
    if (codePoint <= 0x7F) {
        result += static_cast<char>(codePoint);
    } else if (codePoint <= 0x7FF) {
        result += static_cast<char>(0xC0 | (codePoint >> 6));
        result += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint <= 0xFFFF) {
        result += static_cast<char>(0xE0 | (codePoint >> 12));
        result += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint <= 0x10FFFF) {
        result += static_cast<char>(0xF0 | (codePoint >> 18));
        result += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        result += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
    return result;
}

/** Double-check the encoding of the replacementCharacter */
TEST(UnicodeReplacementCharacter, Bytes) {
    ASSERT_EQ(replacementCharacter, (std::string{'\xef', '\xbf', '\xbd'}));
}

const std::vector<std::string> validUTF8Strings{
    "A",
    "\xc2\xa2",          // CENT SIGN: ¢
    "\xe2\x82\xac",      // Euro: €
    "\xf0\x9d\x90\x80",  // Blackboard A: 𝐀
    "\n",
    u8"こんにちは"_as_char_ptr,
    u8"😊"_as_char_ptr,
    "",
};

// Maps invalid UTF-8 to the appropriate scrubbed version.
const std::map<std::string, std::string> scrubMap{
    // Abrupt end
    {"\xc2", repeat(replacementCharacter, 1)},
    {"\xe2\x82", repeat(replacementCharacter, 2)},
    {"\xf0\x9d\x90", repeat(replacementCharacter, 3)},

    // Test having spaces at the end and beginning of scrubbed lines
    {" \xc2 ", " \xef\xbf\xbd "},
    {"\xe2\x82 ", "\xef\xbf\xbd\xef\xbf\xbd "},
    {"\xf0\x9d\x90 ", "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd "},

    // Too long
    {"\xf8\x80\x80\x80\x80", repeat(replacementCharacter, 5)},
    {"\xfc\x80\x80\x80\x80\x80", repeat(replacementCharacter, 6)},
    {"\xfe\x80\x80\x80\x80\x80\x80", repeat(replacementCharacter, 7)},
    {"\xff\x80\x80\x80\x80\x80\x80\x80", repeat(replacementCharacter, 8)},

    {"\x80", repeat(replacementCharacter, 1)},         // Can't start with continuation byte.
    {"\xc3\x28", "\xef\xbf\xbd\x28"},                  // First byte indicates 2-byte sequence, but
                                                       // second byte is not in form 10xxxxxx
    {"\xe2\x28\xa1", "\xef\xbf\xbd\x28\xef\xbf\xbd"},  // first byte indicates 3-byte sequence, but
                                                       // second byte is not in form 10xxxxxx
    {"\xde\xa0\x80",
     "\xde\xa0\xef\xbf\xbd"},  // Surrogate pairs are not valid for UTF-8 (high surrogate)
    {"\xf0\x9d\xdc\x80", "\xef\xbf\xbd\xef\xbf\xbd\xdc\x80"},  // Surrogate pairs are not valid

    // These are invalid UTF-8 strings that currently pass that shouldn't.
    // See SERVER-95394.
    // {"\xf5\x80\x80\x80", repeat(replacementCharacter, 4)},  // U+140000 > U+10FFFF
    // {"\xc0\x80", repeat(replacementCharacter, 2)},          // 2-byte version of ASCII NUL
    // {"\xc1\x80", repeat(replacementCharacter, 2)},           // 2-byte version of ASCII NUL
    // {"\0x88\0x80"}        // Invalid start-byte (0x88 > 0x7f)
};

TEST(StringEscapeTest, ScrubInvalidUTF8) {
    for (auto& in : validUTF8Strings)
        assertCmp(0, str::scrubInvalidUTF8(in), in);
    for (auto& [in, expect] : scrubMap)
        assertCmp(0, str::scrubInvalidUTF8(in), expect);
    for (unsigned i = 0; i < 0x1'0000; ++i) {
        std::string s = codePointToUTF8(i);
        assertCmp(0, scrubInvalidUTF8(s), s);
    }
}

TEST(StringEscapeTest, ValidUTF8) {
    for (auto& str : validUTF8Strings) {
        ASSERT(str::validUTF8(str));
    }
    for (const auto& pair : scrubMap) {
        ASSERT(!str::validUTF8(pair.first));
    }
}

}  // namespace
}  // namespace mongo::str
