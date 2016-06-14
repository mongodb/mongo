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

#include "mongo/db/query/collation/collation_spec.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

TEST(CollationSpecTest, SpecsWithNonEqualLocaleStringsAreNotEqual) {
    CollationSpec collationSpec1;
    collationSpec1.localeID = "fr";

    CollationSpec collationSpec2;
    collationSpec2.localeID = "de";

    ASSERT_FALSE(collationSpec1 == collationSpec2);
    ASSERT_TRUE(collationSpec1 != collationSpec2);
}

TEST(CollationSpecTest, SpecsWithNonEqualCaseLevelValuesAreNotEqual) {
    CollationSpec collationSpec1;
    collationSpec1.localeID = "fr";
    collationSpec1.caseLevel = true;

    CollationSpec collationSpec2;
    collationSpec2.localeID = "fr";
    collationSpec2.caseLevel = false;

    ASSERT_FALSE(collationSpec1 == collationSpec2);
    ASSERT_TRUE(collationSpec1 != collationSpec2);
}

TEST(CollationSpecTest, SpecsWithNonEqualCaseFirstValuesAreNotEqual) {
    CollationSpec collationSpec1;
    collationSpec1.localeID = "fr";
    collationSpec1.caseFirst = CollationSpec::CaseFirstType::kUpper;

    CollationSpec collationSpec2;
    collationSpec2.localeID = "fr";
    collationSpec2.caseFirst = CollationSpec::CaseFirstType::kOff;

    ASSERT_FALSE(collationSpec1 == collationSpec2);
    ASSERT_TRUE(collationSpec1 != collationSpec2);
}

TEST(CollationSpecTest, SpecsWithNonEqualStrengthsAreNotEqual) {
    CollationSpec collationSpec1;
    collationSpec1.localeID = "fr";
    collationSpec1.strength = CollationSpec::StrengthType::kPrimary;

    CollationSpec collationSpec2;
    collationSpec2.localeID = "fr";
    collationSpec2.strength = CollationSpec::StrengthType::kSecondary;

    ASSERT_FALSE(collationSpec1 == collationSpec2);
    ASSERT_TRUE(collationSpec1 != collationSpec2);
}

TEST(CollationSpecTest, SpecsWithNonEqualNumericOrderingValuesAreNotEqual) {
    CollationSpec collationSpec1;
    collationSpec1.localeID = "fr";
    collationSpec1.numericOrdering = false;

    CollationSpec collationSpec2;
    collationSpec2.localeID = "fr";
    collationSpec2.numericOrdering = true;

    ASSERT_FALSE(collationSpec1 == collationSpec2);
    ASSERT_TRUE(collationSpec1 != collationSpec2);
}

TEST(CollationSpecTest, SpecsWithNonEqualAlternateValuesAreNotEqual) {
    CollationSpec collationSpec1;
    collationSpec1.localeID = "fr";
    collationSpec1.alternate = CollationSpec::AlternateType::kNonIgnorable;

    CollationSpec collationSpec2;
    collationSpec2.localeID = "fr";
    collationSpec2.alternate = CollationSpec::AlternateType::kShifted;

    ASSERT_FALSE(collationSpec1 == collationSpec2);
    ASSERT_TRUE(collationSpec1 != collationSpec2);
}

TEST(CollationSpecTest, SpecsWithNonEqualMaxVariableValuesAreNotEqual) {
    CollationSpec collationSpec1;
    collationSpec1.localeID = "fr";
    collationSpec1.maxVariable = CollationSpec::MaxVariableType::kPunct;

    CollationSpec collationSpec2;
    collationSpec2.localeID = "fr";
    collationSpec2.maxVariable = CollationSpec::MaxVariableType::kSpace;

    ASSERT_FALSE(collationSpec1 == collationSpec2);
    ASSERT_TRUE(collationSpec1 != collationSpec2);
}

TEST(CollationSpecTest, SpecsWithNonEqualNormalizationValuesAreNotEqual) {
    CollationSpec collationSpec1;
    collationSpec1.localeID = "fr";
    collationSpec1.normalization = false;

    CollationSpec collationSpec2;
    collationSpec2.localeID = "fr";
    collationSpec2.normalization = true;

    ASSERT_FALSE(collationSpec1 == collationSpec2);
    ASSERT_TRUE(collationSpec1 != collationSpec2);
}

TEST(CollationSpecTest, SpecsWithNonEqualBackwardsValuesAreNotEqual) {
    CollationSpec collationSpec1;
    collationSpec1.localeID = "fr";
    collationSpec1.backwards = false;

    CollationSpec collationSpec2;
    collationSpec2.localeID = "fr";
    collationSpec2.backwards = true;

    ASSERT_FALSE(collationSpec1 == collationSpec2);
    ASSERT_TRUE(collationSpec1 != collationSpec2);
}

TEST(CollationSpecTest, SpecsWithNonEqualVersionValuesAreNotEqual) {
    CollationSpec collationSpec1;
    collationSpec1.localeID = "fr";
    collationSpec1.version = "version1";

    CollationSpec collationSpec2;
    collationSpec2.localeID = "fr";
    collationSpec2.version = "version2";

    ASSERT_FALSE(collationSpec1 == collationSpec2);
    ASSERT_TRUE(collationSpec1 != collationSpec2);
}

TEST(CollationSpecTest, EqualSpecs) {
    CollationSpec collationSpec1;
    collationSpec1.localeID = "fr";

    CollationSpec collationSpec2;
    collationSpec2.localeID = "fr";

    ASSERT_TRUE(collationSpec1 == collationSpec2);
    ASSERT_FALSE(collationSpec1 != collationSpec2);
}

TEST(CollationSpecTest, ToBSONCorrectlySerializesDefaults) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";
    collationSpec.version = "myVersion";

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel"
                               << false
                               << "caseFirst"
                               << "off"
                               << "strength"
                               << 3
                               << "numericOrdering"
                               << false
                               << "alternate"
                               << "non-ignorable"
                               << "maxVariable"
                               << "punct"
                               << "normalization"
                               << false
                               << "backwards"
                               << false
                               << "version"
                               << "myVersion");

    ASSERT_EQ(expectedObj, collationSpec.toBSON());
}

TEST(CollationSpecTest, ToBSONCorrectlySerializesCaseFirstUpper) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";
    collationSpec.version = "myVersion";
    collationSpec.caseFirst = CollationSpec::CaseFirstType::kUpper;

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel"
                               << false
                               << "caseFirst"
                               << "upper"
                               << "strength"
                               << 3
                               << "numericOrdering"
                               << false
                               << "alternate"
                               << "non-ignorable"
                               << "maxVariable"
                               << "punct"
                               << "normalization"
                               << false
                               << "backwards"
                               << false
                               << "version"
                               << "myVersion");

    ASSERT_EQ(expectedObj, collationSpec.toBSON());
}

TEST(CollationSpecTest, ToBSONCorrectlySerializesCaseFirstLower) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";
    collationSpec.version = "myVersion";
    collationSpec.caseFirst = CollationSpec::CaseFirstType::kLower;

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel"
                               << false
                               << "caseFirst"
                               << "lower"
                               << "strength"
                               << 3
                               << "numericOrdering"
                               << false
                               << "alternate"
                               << "non-ignorable"
                               << "maxVariable"
                               << "punct"
                               << "normalization"
                               << false
                               << "backwards"
                               << false
                               << "version"
                               << "myVersion");

    ASSERT_EQ(expectedObj, collationSpec.toBSON());
}

TEST(CollationSpecTest, ToBSONCorrectlySerializesPrimaryStrength) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";
    collationSpec.version = "myVersion";
    collationSpec.strength = CollationSpec::StrengthType::kPrimary;

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel"
                               << false
                               << "caseFirst"
                               << "off"
                               << "strength"
                               << 1
                               << "numericOrdering"
                               << false
                               << "alternate"
                               << "non-ignorable"
                               << "maxVariable"
                               << "punct"
                               << "normalization"
                               << false
                               << "backwards"
                               << false
                               << "version"
                               << "myVersion");

    ASSERT_EQ(expectedObj, collationSpec.toBSON());
}

TEST(CollationSpecTest, ToBSONCorrectlySerializesSecondaryStrength) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";
    collationSpec.version = "myVersion";
    collationSpec.strength = CollationSpec::StrengthType::kSecondary;

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel"
                               << false
                               << "caseFirst"
                               << "off"
                               << "strength"
                               << 2
                               << "numericOrdering"
                               << false
                               << "alternate"
                               << "non-ignorable"
                               << "maxVariable"
                               << "punct"
                               << "normalization"
                               << false
                               << "backwards"
                               << false
                               << "version"
                               << "myVersion");

    ASSERT_EQ(expectedObj, collationSpec.toBSON());
}

TEST(CollationSpecTest, ToBSONCorrectlySerializesQuaternaryStrength) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";
    collationSpec.version = "myVersion";
    collationSpec.strength = CollationSpec::StrengthType::kQuaternary;

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel"
                               << false
                               << "caseFirst"
                               << "off"
                               << "strength"
                               << 4
                               << "numericOrdering"
                               << false
                               << "alternate"
                               << "non-ignorable"
                               << "maxVariable"
                               << "punct"
                               << "normalization"
                               << false
                               << "backwards"
                               << false
                               << "version"
                               << "myVersion");

    ASSERT_EQ(expectedObj, collationSpec.toBSON());
}

TEST(CollationSpecTest, ToBSONCorrectlySerializesIdenticalStrength) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";
    collationSpec.version = "myVersion";
    collationSpec.strength = CollationSpec::StrengthType::kIdentical;

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel"
                               << false
                               << "caseFirst"
                               << "off"
                               << "strength"
                               << 5
                               << "numericOrdering"
                               << false
                               << "alternate"
                               << "non-ignorable"
                               << "maxVariable"
                               << "punct"
                               << "normalization"
                               << false
                               << "backwards"
                               << false
                               << "version"
                               << "myVersion");

    ASSERT_EQ(expectedObj, collationSpec.toBSON());
}

TEST(CollationSpecTest, ToBSONCorrectlySerializesAlternateShifted) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";
    collationSpec.version = "myVersion";
    collationSpec.alternate = CollationSpec::AlternateType::kShifted;

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel"
                               << false
                               << "caseFirst"
                               << "off"
                               << "strength"
                               << 3
                               << "numericOrdering"
                               << false
                               << "alternate"
                               << "shifted"
                               << "maxVariable"
                               << "punct"
                               << "normalization"
                               << false
                               << "backwards"
                               << false
                               << "version"
                               << "myVersion");

    ASSERT_EQ(expectedObj, collationSpec.toBSON());
}

TEST(CollationSpecTest, ToBSONCorrectlySerializesMaxVariableSpace) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";
    collationSpec.version = "myVersion";
    collationSpec.maxVariable = CollationSpec::MaxVariableType::kSpace;

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel"
                               << false
                               << "caseFirst"
                               << "off"
                               << "strength"
                               << 3
                               << "numericOrdering"
                               << false
                               << "alternate"
                               << "non-ignorable"
                               << "maxVariable"
                               << "space"
                               << "normalization"
                               << false
                               << "backwards"
                               << false
                               << "version"
                               << "myVersion");

    ASSERT_EQ(expectedObj, collationSpec.toBSON());
}

}  // namespace
