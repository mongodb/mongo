/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/query/collation/collator_interface_icu.h"

#include <iomanip>
#include <iostream>
#include <unicode/coll.h>

#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

enum class ExpectedComparison {
    EQUAL,
    NOT_EQUAL,
    LESS_THAN,
    GREATER_THAN,
};

bool isExpectedComparison(int cmp, ExpectedComparison expectedCmp) {
    switch (expectedCmp) {
        case ExpectedComparison::EQUAL:
            return cmp == 0;
        case ExpectedComparison::NOT_EQUAL:
            return cmp != 0;
        case ExpectedComparison::LESS_THAN:
            return cmp < 0;
        case ExpectedComparison::GREATER_THAN:
            return cmp > 0;
    }

    MONGO_UNREACHABLE;
}

void assertEnUSComparison(StringData left, StringData right, ExpectedComparison expectedCmp) {
    CollationSpec collationSpec;
    collationSpec.localeID = "en_US";
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll(
        icu::Collator::createInstance(icu::Locale("en", "US"), status));
    ASSERT(U_SUCCESS(status));
    CollatorInterfaceICU icuCollator(collationSpec, std::move(coll));

    ASSERT(isExpectedComparison(icuCollator.compare(left, right), expectedCmp));

    const auto leftKey = icuCollator.getComparisonKey(left);
    const auto rightKey = icuCollator.getComparisonKey(right);
    ASSERT(isExpectedComparison(leftKey.getKeyData().compare(rightKey.getKeyData()), expectedCmp));
}

// Returns true if an ICU collator compares 'lessThan' as smaller than 'greaterThan'. Verifies that
// the comparison is correct using the compare() function directly and using comparison keys.
void assertLessThanEnUS(StringData lessThan, StringData greaterThan) {
    assertEnUSComparison(lessThan, greaterThan, ExpectedComparison::LESS_THAN);
    assertEnUSComparison(greaterThan, lessThan, ExpectedComparison::GREATER_THAN);
}

// Returns true if an ICU collator compares 'left' and 'right' as equal. Verifies that
// the comparison is correct using the compare() function directly and using comparison keys.
void assertEqualEnUS(StringData left, StringData right) {
    assertEnUSComparison(left, right, ExpectedComparison::EQUAL);
    assertEnUSComparison(right, left, ExpectedComparison::EQUAL);
}

// Returns true if an ICU collator compares 'left' and 'right' as non-equal. Verifies that
// the comparison is correct using the compare() function directly and using comparison keys.
void assertNotEqualEnUS(StringData left, StringData right) {
    assertEnUSComparison(left, right, ExpectedComparison::NOT_EQUAL);
    assertEnUSComparison(right, left, ExpectedComparison::NOT_EQUAL);
}

TEST(CollatorInterfaceICUTest, ClonedCollatorMatchesOriginal) {
    CollationSpec collationSpec;
    collationSpec.localeID = "en_US";

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll(
        icu::Collator::createInstance(icu::Locale("en", "US"), status));
    ASSERT(U_SUCCESS(status));

    CollatorInterfaceICU icuCollator(collationSpec, std::move(coll));
    auto clone = icuCollator.clone();
    ASSERT_TRUE(*clone == icuCollator);
}

TEST(CollatorInterfaceICUTest, ASCIIComparisonWorksForUSEnglishCollation) {
    CollationSpec collationSpec;
    collationSpec.localeID = "en_US";

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll(
        icu::Collator::createInstance(icu::Locale("en", "US"), status));
    ASSERT(U_SUCCESS(status));

    CollatorInterfaceICU icuCollator(collationSpec, std::move(coll));
    ASSERT_LT(icuCollator.compare("ab", "ba"), 0);
    ASSERT_GT(icuCollator.compare("ba", "ab"), 0);
    ASSERT_EQ(icuCollator.compare("ab", "ab"), 0);
}

TEST(CollatorInterfaceICUTest, ASCIIComparisonWorksUsingLocaleStringParsing) {
    CollationSpec collationSpec;
    collationSpec.localeID = "en_US";

    auto locale = icu::Locale::createFromName(collationSpec.localeID.c_str());
    ASSERT_EQ(std::string("en"), locale.getLanguage());
    ASSERT_EQ(std::string("US"), locale.getCountry());

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll(icu::Collator::createInstance(locale, status));
    ASSERT(U_SUCCESS(status));

    CollatorInterfaceICU icuCollator(collationSpec, std::move(coll));
    ASSERT_LT(icuCollator.compare("ab", "ba"), 0);
    ASSERT_GT(icuCollator.compare("ba", "ab"), 0);
    ASSERT_EQ(icuCollator.compare("ab", "ab"), 0);
}

TEST(CollatorInterfaceICUTest, ASCIIComparisonWorksUsingComparisonKeys) {
    CollationSpec collationSpec;
    collationSpec.localeID = "en_US";

    auto locale = icu::Locale::createFromName(collationSpec.localeID.c_str());
    ASSERT_EQ(std::string("en"), locale.getLanguage());
    ASSERT_EQ(std::string("US"), locale.getCountry());

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll(icu::Collator::createInstance(locale, status));
    ASSERT(U_SUCCESS(status));

    CollatorInterfaceICU icuCollator(collationSpec, std::move(coll));
    const auto comparisonKeyAB = icuCollator.getComparisonKey("ab");
    const auto comparisonKeyABB = icuCollator.getComparisonKey("abb");
    const auto comparisonKeyBA = icuCollator.getComparisonKey("ba");

    ASSERT_LT(comparisonKeyAB.getKeyData().compare(comparisonKeyBA.getKeyData()), 0);
    ASSERT_GT(comparisonKeyBA.getKeyData().compare(comparisonKeyAB.getKeyData()), 0);
    ASSERT_EQ(comparisonKeyAB.getKeyData().compare(comparisonKeyAB.getKeyData()), 0);

    ASSERT_LT(comparisonKeyAB.getKeyData().compare(comparisonKeyABB.getKeyData()), 0);
    ASSERT_GT(comparisonKeyABB.getKeyData().compare(comparisonKeyAB.getKeyData()), 0);

    ASSERT_GT(comparisonKeyBA.getKeyData().compare(comparisonKeyABB.getKeyData()), 0);
    ASSERT_LT(comparisonKeyABB.getKeyData().compare(comparisonKeyBA.getKeyData()), 0);
}

TEST(CollatorInterfaceICUTest, ZeroLengthStringsCompareCorrectly) {
    CollationSpec collationSpec;
    collationSpec.localeID = "en_US";

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll(
        icu::Collator::createInstance(icu::Locale("en", "US"), status));
    ASSERT(U_SUCCESS(status));

    CollatorInterfaceICU icuCollator(collationSpec, std::move(coll));
    ASSERT_EQ(icuCollator.compare(StringData(), StringData()), 0);
    ASSERT_LT(icuCollator.compare(StringData(), "abc"), 0);
    ASSERT_GT(icuCollator.compare("abc", StringData()), 0);
}

TEST(CollatorInterfaceICUTest, ZeroLengthStringsCompareCorrectlyUsingComparisonKeys) {
    CollationSpec collationSpec;
    collationSpec.localeID = "en_US";

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll(
        icu::Collator::createInstance(icu::Locale("en", "US"), status));
    ASSERT(U_SUCCESS(status));

    CollatorInterfaceICU icuCollator(collationSpec, std::move(coll));
    auto emptyKey = icuCollator.getComparisonKey(StringData());
    auto comparisonKeyABC = icuCollator.getComparisonKey("abc");
    ASSERT_EQ(emptyKey.getKeyData().compare(emptyKey.getKeyData()), 0);
    ASSERT_LT(emptyKey.getKeyData().compare(comparisonKeyABC.getKeyData()), 0);
    ASSERT_GT(comparisonKeyABC.getKeyData().compare(emptyKey.getKeyData()), 0);
}

TEST(CollatorInterfaceICUTest, EmptyNullTerminatedStringComparesCorrectly) {
    CollationSpec collationSpec;
    collationSpec.localeID = "en_US";

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll(
        icu::Collator::createInstance(icu::Locale("en", "US"), status));
    ASSERT(U_SUCCESS(status));

    StringData emptyString("");
    ASSERT(emptyString.rawData());
    ASSERT_EQ(emptyString.size(), 0u);

    CollatorInterfaceICU icuCollator(collationSpec, std::move(coll));
    ASSERT_EQ(icuCollator.compare(emptyString, emptyString), 0);
    ASSERT_LT(icuCollator.compare(emptyString, "abc"), 0);
    ASSERT_GT(icuCollator.compare("abc", emptyString), 0);
}

TEST(CollatorInterfaceICUTest, EmptyNullTerminatedStringComparesCorrectlyUsingComparisonKeys) {
    CollationSpec collationSpec;
    collationSpec.localeID = "en_US";

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll(
        icu::Collator::createInstance(icu::Locale("en", "US"), status));
    ASSERT(U_SUCCESS(status));

    StringData emptyString("");
    ASSERT(emptyString.rawData());
    ASSERT_EQ(emptyString.size(), 0u);

    CollatorInterfaceICU icuCollator(collationSpec, std::move(coll));
    auto emptyKey = icuCollator.getComparisonKey(emptyString);
    auto comparisonKeyABC = icuCollator.getComparisonKey("abc");
    ASSERT_EQ(emptyKey.getKeyData().compare(emptyKey.getKeyData()), 0);
    ASSERT_LT(emptyKey.getKeyData().compare(comparisonKeyABC.getKeyData()), 0);
    ASSERT_GT(comparisonKeyABC.getKeyData().compare(emptyKey.getKeyData()), 0);
}

TEST(CollatorInterfaceICUTest, LengthOneStringWithNullByteComparesCorrectly) {
    CollationSpec collationSpec;
    collationSpec.localeID = "en_US";

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll(
        icu::Collator::createInstance(icu::Locale("en", "US"), status));
    ASSERT(U_SUCCESS(status));

    const auto nullByte = "\0"_sd;
    ASSERT_EQ(nullByte.rawData()[0], '\0');
    ASSERT_EQ(nullByte.size(), 1u);

    CollatorInterfaceICU icuCollator(collationSpec, std::move(coll));
    ASSERT_EQ(icuCollator.compare(nullByte, nullByte), 0);
    ASSERT_LT(icuCollator.compare(nullByte, "abc"), 0);
    ASSERT_GT(icuCollator.compare("abc", nullByte), 0);
}

TEST(CollatorInterfaceICUTest, LengthOneStringWithNullByteComparesCorrectlyUsingComparisonKeys) {
    CollationSpec collationSpec;
    collationSpec.localeID = "en_US";

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll(
        icu::Collator::createInstance(icu::Locale("en", "US"), status));
    ASSERT(U_SUCCESS(status));

    const auto nullByte = "\0"_sd;
    ASSERT_EQ(nullByte.rawData()[0], '\0');
    ASSERT_EQ(nullByte.size(), 1u);

    CollatorInterfaceICU icuCollator(collationSpec, std::move(coll));
    auto nullByteKey = icuCollator.getComparisonKey(nullByte);
    auto comparisonKeyABC = icuCollator.getComparisonKey("abc");
    ASSERT_EQ(nullByteKey.getKeyData().compare(nullByteKey.getKeyData()), 0);
    ASSERT_LT(nullByteKey.getKeyData().compare(comparisonKeyABC.getKeyData()), 0);
    ASSERT_GT(comparisonKeyABC.getKeyData().compare(nullByteKey.getKeyData()), 0);
}

TEST(CollatorInterfaceICUTest, StringsWithEmbeddedNullByteCompareCorrectly) {
    CollationSpec collationSpec;
    collationSpec.localeID = "en_US";

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll(
        icu::Collator::createInstance(icu::Locale("en", "US"), status));
    ASSERT(U_SUCCESS(status));

    const auto string1 = "a\0b"_sd;
    ASSERT_EQ(string1.size(), 3u);
    const auto string2 = "a\0c"_sd;
    ASSERT_EQ(string2.size(), 3u);

    CollatorInterfaceICU icuCollator(collationSpec, std::move(coll));
    ASSERT_EQ(icuCollator.compare(string1, string1), 0);
    ASSERT_LT(icuCollator.compare(string1, string2), 0);
    ASSERT_GT(icuCollator.compare(string2, string1), 0);
}

TEST(CollatorInterfaceICUTest, StringsWithEmbeddedNullByteCompareCorrectlyUsingComparisonKeys) {
    CollationSpec collationSpec;
    collationSpec.localeID = "en_US";

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll(
        icu::Collator::createInstance(icu::Locale("en", "US"), status));
    ASSERT(U_SUCCESS(status));

    const auto string1 = "a\0b"_sd;
    ASSERT_EQ(string1.size(), 3u);
    const auto string2 = "a\0c"_sd;
    ASSERT_EQ(string2.size(), 3u);

    CollatorInterfaceICU icuCollator(collationSpec, std::move(coll));
    auto key1 = icuCollator.getComparisonKey(string1);
    auto key2 = icuCollator.getComparisonKey(string2);
    ASSERT_EQ(key1.getKeyData().compare(key1.getKeyData()), 0);
    ASSERT_LT(key1.getKeyData().compare(key2.getKeyData()), 0);
    ASSERT_GT(key2.getKeyData().compare(key1.getKeyData()), 0);
}

TEST(CollatorInterfaceICUTest, TwoUSEnglishCollationsAreEqual) {
    CollationSpec collationSpec;
    collationSpec.localeID = "en_US";
    auto locale = icu::Locale::createFromName(collationSpec.localeID.c_str());

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll1(icu::Collator::createInstance(locale, status));
    ASSERT(U_SUCCESS(status));

    std::unique_ptr<icu::Collator> coll2(icu::Collator::createInstance(locale, status));
    ASSERT(U_SUCCESS(status));

    CollatorInterfaceICU icuCollator1(collationSpec, std::move(coll1));
    CollatorInterfaceICU icuCollator2(collationSpec, std::move(coll2));
    ASSERT_TRUE(icuCollator1 == icuCollator2);
    ASSERT_FALSE(icuCollator1 != icuCollator2);
}

TEST(CollatorInterfaceICUTest, USEnglishAndBritishEnglishCollationsAreNotEqual) {
    CollationSpec collationSpec1;
    collationSpec1.localeID = "en_US";
    auto locale1 = icu::Locale::createFromName(collationSpec1.localeID.c_str());

    CollationSpec collationSpec2;
    collationSpec2.localeID = "en_UK";
    auto locale2 = icu::Locale::createFromName(collationSpec2.localeID.c_str());

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll1(icu::Collator::createInstance(locale1, status));
    ASSERT(U_SUCCESS(status));

    std::unique_ptr<icu::Collator> coll2(icu::Collator::createInstance(locale2, status));
    ASSERT(U_SUCCESS(status));

    CollatorInterfaceICU icuCollator1(collationSpec1, std::move(coll1));
    CollatorInterfaceICU icuCollator2(collationSpec2, std::move(coll2));
    ASSERT_FALSE(icuCollator1 == icuCollator2);
    ASSERT_TRUE(icuCollator1 != icuCollator2);
}

TEST(CollatorInterfaceICUTest, FrenchCanadianCollatorComparesCorrectly) {
    CollationSpec collationSpec;
    collationSpec.localeID = "fr_CA";

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll(
        icu::Collator::createInstance(icu::Locale("fr", "CA"), status));
    ASSERT(U_SUCCESS(status));

    CollatorInterfaceICU icuCollator(collationSpec, std::move(coll));

    StringData circumflex(u8"p\u00EAche");
    StringData graveAndAcute(u8"p\u00E8ch\u00E9");
    StringData circumflexAndAcute(u8"p\u00EAch\u00E9");

    ASSERT_LT(icuCollator.compare(circumflex, graveAndAcute), 0);
    ASSERT_LT(icuCollator.compare(graveAndAcute, circumflexAndAcute), 0);
    ASSERT_LT(icuCollator.compare(circumflex, circumflexAndAcute), 0);

    ASSERT_GT(icuCollator.compare(circumflexAndAcute, graveAndAcute), 0);
    ASSERT_GT(icuCollator.compare(graveAndAcute, circumflex), 0);
    ASSERT_GT(icuCollator.compare(circumflexAndAcute, circumflex), 0);
}

TEST(CollatorInterfaceICUTest, FrenchCanadianCollatorComparesCorrectlyUsingComparisonKeys) {
    CollationSpec collationSpec;
    collationSpec.localeID = "fr_CA";

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll(
        icu::Collator::createInstance(icu::Locale("fr", "CA"), status));
    ASSERT(U_SUCCESS(status));

    CollatorInterfaceICU icuCollator(collationSpec, std::move(coll));

    auto circumflex = icuCollator.getComparisonKey(u8"p\u00EAche");
    auto graveAndAcute = icuCollator.getComparisonKey(u8"p\u00E8ch\u00E9");
    auto circumflexAndAcute = icuCollator.getComparisonKey(u8"p\u00EAch\u00E9");

    ASSERT_LT(circumflex.getKeyData().compare(graveAndAcute.getKeyData()), 0);
    ASSERT_LT(graveAndAcute.getKeyData().compare(circumflexAndAcute.getKeyData()), 0);
    ASSERT_LT(circumflex.getKeyData().compare(circumflexAndAcute.getKeyData()), 0);

    ASSERT_GT(circumflexAndAcute.getKeyData().compare(graveAndAcute.getKeyData()), 0);
    ASSERT_GT(graveAndAcute.getKeyData().compare(circumflex.getKeyData()), 0);
    ASSERT_GT(circumflexAndAcute.getKeyData().compare(circumflex.getKeyData()), 0);
}

TEST(CollatorInterfaceICUTest, InvalidOneByteSequencesCompareEqual) {
    // Both one-byte sequences are invalid.
    assertEqualEnUS("\xEF", "\xF2");
}

TEST(CollatorInterfaceICUTest, LonelyStartCharacterComparesEqualToReplacementCharacter) {
    assertEqualEnUS("\xEF", u8"\uFFFD");
}

TEST(CollatorInterfaceICUTest, ThreeByteSeqWithLastByteMissingComparesEqualToReplacement) {
    // U+0823 ("samaritan vowel sign a") with last byte missing.
    assertEqualEnUS("\xE0\xA0", u8"\uFFFD");
}

TEST(CollatorInterfaceICUTest, InvalidOneByteSeqAndTwoByteSeqCompareEqual) {
    // Impossible byte compared against U+0823 ("samaritan vowel sign a") with last byte missing.
    assertEqualEnUS("\xEF", "\xE0\xA0");
}

TEST(CollatorInterfaceICUTest, OverlongASCIICharacterComparesEqualToReplacementCharacter) {
    // U+002F is the ASCII character "/", which should usually be represented as \x2F. The
    // representation \xC0\xAF is an unnecessary two-byte encoding of this codepoint.
    assertEqualEnUS("\xC0\xAF", u8"\uFFFD");
}

TEST(CollatorInterfaceICUTest, OverlongNullComparesEqualToReplacementCharacter) {
    // The two-byte sequence \xC0\x80 decodes to U+0000, which should instead be encoded using a
    // single null byte.
    assertEqualEnUS("\xC0\x80", u8"\uFFFD");
}

TEST(CollatorInterfaceICUTest, IllegalCodePositionsCompareEqualToReplacementCharacter) {
    // U+D800
    assertEqualEnUS("\xED\xA0\x80", u8"\uFFFD");
    // U+DBFF
    assertEqualEnUS("\xED\xAF\xBF", u8"\uFFFD");
    // U+DFFF
    assertEqualEnUS("\xED\xBF\xBF", u8"\uFFFD");
    // U+D800, U+DC00
    assertEqualEnUS("\xED\xA0\x80\xED\xB0\x80", u8"\uFFFD\uFFFD");
    // U+DB80, U+DFFF
    assertEqualEnUS("\xED\xAE\x80\xED\xBF\xBF", u8"\uFFFD\uFFFD");
}

TEST(CollatorInterfaceICUTest, UnexpectedTrailingContinuationByteComparesAsReplacementCharacter) {
    // U+0123 ("latin small letter g with cedilla").
    StringData gWithCedilla("\xC4\xA3");
    // U+0123 ("latin small letter g with cedilla") with an unexpected continuation byte.
    StringData unexpectedContinuation("\xC4\xA3\x80");
    // U+0123 ("latin small letter g with cedilla") followed by the replacement character, U+FFFD.
    StringData gWithCedillaPlusReplacement("\xC4\xA3\xEF\xBF\xBD");

    assertLessThanEnUS(gWithCedilla, unexpectedContinuation);
    assertLessThanEnUS(gWithCedilla, gWithCedillaPlusReplacement);
    assertEqualEnUS(unexpectedContinuation, gWithCedillaPlusReplacement);
}

TEST(CollatorInterfaceICUTest, ImpossibleBytesCompareEqualToReplacementCharacter) {
    assertEqualEnUS("\xFE", u8"\uFFFD");
    assertEqualEnUS("\xFF", u8"\uFFFD");
}

TEST(CollatorInterfaceICUTest, FourImpossibleBytesCompareEqualToFourReplacementCharacters) {
    assertEqualEnUS("\xFE\xFE\xFF\xFF", u8"\uFFFD\uFFFD\uFFFD\uFFFD");
}

TEST(CollatorInterfaceICUTest, TwoUnexpectedContinuationsCompareAsTwoReplacementCharacters) {
    // U+0123 ("latin small letter g with cedilla").
    StringData gWithCedilla("\xC4\xA3");
    // U+0123 ("latin small letter g with cedilla") with two unexpected continuation bytes.
    StringData unexpectedContinuations("\xC4\xA3\x80\x80");
    // U+0123 ("latin small letter g with cedilla") followed by two replacement characters.
    StringData gWithCedillaPlusReplacements(u8"\u0123\uFFFD\uFFFD");

    assertLessThanEnUS(gWithCedilla, unexpectedContinuations);
    assertLessThanEnUS(gWithCedilla, gWithCedillaPlusReplacements);
    assertEqualEnUS(unexpectedContinuations, gWithCedillaPlusReplacements);
}

TEST(CollatorInterfaceICUTest, FirstPossibleSequenceOfLengthNotEqualToReplacementCharacter) {
    // First possible valid one-byte code point, U+0000.
    assertNotEqualEnUS("\x00", u8"\uFFFD");
    // First possible valid two-byte code point, U+0080.
    assertNotEqualEnUS("\xC2\x80", u8"\uFFFD");
    // First possible valid three-byte code point, U+0800.
    assertNotEqualEnUS("\xE0\xA0\x80", u8"\uFFFD");
    // First possible valid four-byte code point, U+00010000.
    assertNotEqualEnUS("\xF0\x90\x80\x80", u8"\uFFFD");
}

TEST(CollatorInterfaceICUTest, LastPossibleSequenceOfLengthNotEqualToReplacementCharacter) {
    // Last possible valid one-byte code point, U+007F.
    assertNotEqualEnUS("\x7F", u8"\uFFFD");
    // Last possible valid two-byte code point, U+07FF.
    assertNotEqualEnUS("\xDF\xBF", u8"\uFFFD");
    // Last possible valid three-byte code point, U+FFFF.
    assertNotEqualEnUS("\xEF\xBF\xBF", u8"\uFFFD");
    // Largest valid code point, U+0010FFFF.
    assertNotEqualEnUS("\xF4\x8F\xBF\xBF", u8"\uFFFD");
}

TEST(CollatorInterfaceICUTest, CodePointBeyondLargestValidComparesEqualToReplacementCharacter) {
    // Largest valid code point is U+0010FFFF; U+001FFFFF is higher, and is the last possible valid
    // four byte sequence.
    assertEqualEnUS("\xF7\xBF\xBF\xBF", u8"\uFFFD");
}

TEST(CollatorInterfaceICUTest, StringsWithDifferentEmbeddedInvalidSequencesCompareEqual) {
    // U+0123 ("latin small letter g with cedilla"), follwed by invalid byte \xEF, followed by
    // U+0145 ("latin capital letter n with cedilla").
    StringData invalid1("\xC4\xA3\xEF\xC5\x85");
    // U+0123 ("latin small letter g with cedilla"), follwed by unexpected continuation byte \x80,
    // followed by U+0145 ("latin capital letter n with cedilla").
    StringData invalid2("\xC4\xA3\x80\xC5\x85");
    // U+0123 ("latin small letter g with cedilla"), followed by the replacement character, followed
    // by U+0145 ("latin capital letter n with cedilla").
    StringData withReplacementChar(u8"\u0123\uFFFD\u0145");

    assertEqualEnUS(invalid1, invalid2);
    assertEqualEnUS(invalid1, withReplacementChar);
    assertEqualEnUS(invalid2, withReplacementChar);
}

TEST(CollatorInterfaceICUTest, DifferentEmbeddedInvalidSequencesAndDifferentFinalCodePoints) {
    // U+0123 ("latin small letter g with cedilla"), followed by U+0146 ("latin small letter n
    // with cedilla").
    StringData valid1("\xC4\xA3\xC5\x86");
    // U+0123 ("latin small letter g with cedilla"), followed by U+0145 ("latin capital letter n
    // with cedilla").
    StringData valid2("\xC4\xA3\xC5\x85");

    // U+0123 ("latin small letter g with cedilla"), follwed by unexpected continuation byte \x80,
    // followed by U+0146 ("latin small letter n with cedilla").
    StringData invalid1("\xC4\xA3\x80\xC5\x86");
    // U+0123 ("latin small letter g with cedilla"), follwed by invalid byte \xEF, followed by
    // U+0145 ("latin capital letter n with cedilla").
    StringData invalid2("\xC4\xA3\xEF\xC5\x85");

    assertLessThanEnUS(valid1, valid2);
    assertLessThanEnUS(invalid1, invalid2);

    // Invalid strings are always greater than valid strings, since the replacement character (which
    // replaces the invalid byte) compares greater than U+0145 and U+0146.
    assertLessThanEnUS(valid1, invalid1);
    assertLessThanEnUS(valid1, invalid2);
    assertLessThanEnUS(valid2, invalid1);
    assertLessThanEnUS(valid2, invalid2);
}

TEST(CollatorInterfaceICUTest, ComparisonKeysForEnUsCollatorCorrect) {
    CollationSpec collationSpec;
    collationSpec.localeID = "en_US";
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll(
        icu::Collator::createInstance(icu::Locale("en", "US"), status));
    ASSERT(U_SUCCESS(status));
    CollatorInterfaceICU icuCollator(collationSpec, std::move(coll));

    ASSERT_EQ(icuCollator.getComparisonKey("abc").getKeyData(), "\x29\x2B\x2D\x01\x07\x01\x07");
    ASSERT_EQ(icuCollator.getComparisonKey("c\xC3\xB4t\xC3\xA9").getKeyData(),
              "\x2D\x45\x4F\x31\x01\x44\x8E\x44\x88\x01\x0A");
}

TEST(CollatorInterfaceICUTest, ComparisonKeysForFrCaCollatorCorrect) {
    CollationSpec collationSpec;
    collationSpec.localeID = "fr_CA";
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> coll(
        icu::Collator::createInstance(icu::Locale("fr", "CA"), status));
    ASSERT(U_SUCCESS(status));
    CollatorInterfaceICU icuCollator(collationSpec, std::move(coll));

    ASSERT_EQ(icuCollator.getComparisonKey("abc").getKeyData(), "\x29\x2B\x2D\x01\x07\x01\x07");
    ASSERT_EQ(icuCollator.getComparisonKey("c\xC3\xB4t\xC3\xA9").getKeyData(),
              "\x2D\x45\x4F\x31\x01\x88\x44\x8E\x06\x01\x0A");
}

}  // namespace
