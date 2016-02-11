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

}  // namespace
