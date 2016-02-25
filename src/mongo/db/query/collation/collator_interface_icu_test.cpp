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

#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

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

    StringData nullByte("\0", StringData::LiteralTag());
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

    StringData nullByte("\0", StringData::LiteralTag());
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

    StringData string1("a\0b", StringData::LiteralTag());
    ASSERT_EQ(string1.size(), 3u);
    StringData string2("a\0c", StringData::LiteralTag());
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

    StringData string1("a\0b", StringData::LiteralTag());
    ASSERT_EQ(string1.size(), 3u);
    StringData string2("a\0c", StringData::LiteralTag());
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

}  // namespace
