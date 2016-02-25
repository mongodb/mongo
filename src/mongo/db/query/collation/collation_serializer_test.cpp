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

#include "mongo/db/query/collation/collation_serializer.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

TEST(CollationSerializerTest, ToBSONCorrectlySerializesDefaults) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel" << false << "caseFirst"
                               << "off"
                               << "strength" << 3 << "numericOrdering" << false << "alternate"
                               << "non-ignorable"
                               << "maxVariable"
                               << "punct"
                               << "normalization" << false << "backwards" << false);

    ASSERT_EQ(expectedObj, CollationSerializer::specToBSON(collationSpec));
}

TEST(CollationSerializerTest, ToBSONCorrectlySerializesCaseFirstUpper) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";
    collationSpec.caseFirst = CollationSpec::CaseFirstType::kUpper;

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel" << false << "caseFirst"
                               << "upper"
                               << "strength" << 3 << "numericOrdering" << false << "alternate"
                               << "non-ignorable"
                               << "maxVariable"
                               << "punct"
                               << "normalization" << false << "backwards" << false);

    ASSERT_EQ(expectedObj, CollationSerializer::specToBSON(collationSpec));
}

TEST(CollationSerializerTest, ToBSONCorrectlySerializesCaseFirstLower) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";
    collationSpec.caseFirst = CollationSpec::CaseFirstType::kLower;

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel" << false << "caseFirst"
                               << "lower"
                               << "strength" << 3 << "numericOrdering" << false << "alternate"
                               << "non-ignorable"
                               << "maxVariable"
                               << "punct"
                               << "normalization" << false << "backwards" << false);

    ASSERT_EQ(expectedObj, CollationSerializer::specToBSON(collationSpec));
}

TEST(CollationSerializerTest, ToBSONCorrectlySerializesPrimaryStrength) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";
    collationSpec.strength = CollationSpec::StrengthType::kPrimary;

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel" << false << "caseFirst"
                               << "off"
                               << "strength" << 1 << "numericOrdering" << false << "alternate"
                               << "non-ignorable"
                               << "maxVariable"
                               << "punct"
                               << "normalization" << false << "backwards" << false);

    ASSERT_EQ(expectedObj, CollationSerializer::specToBSON(collationSpec));
}

TEST(CollationSerializerTest, ToBSONCorrectlySerializesSecondaryStrength) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";
    collationSpec.strength = CollationSpec::StrengthType::kSecondary;

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel" << false << "caseFirst"
                               << "off"
                               << "strength" << 2 << "numericOrdering" << false << "alternate"
                               << "non-ignorable"
                               << "maxVariable"
                               << "punct"
                               << "normalization" << false << "backwards" << false);

    ASSERT_EQ(expectedObj, CollationSerializer::specToBSON(collationSpec));
}

TEST(CollationSerializerTest, ToBSONCorrectlySerializesQuaternaryStrength) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";
    collationSpec.strength = CollationSpec::StrengthType::kQuaternary;

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel" << false << "caseFirst"
                               << "off"
                               << "strength" << 4 << "numericOrdering" << false << "alternate"
                               << "non-ignorable"
                               << "maxVariable"
                               << "punct"
                               << "normalization" << false << "backwards" << false);

    ASSERT_EQ(expectedObj, CollationSerializer::specToBSON(collationSpec));
}

TEST(CollationSerializerTest, ToBSONCorrectlySerializesIdenticalStrength) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";
    collationSpec.strength = CollationSpec::StrengthType::kIdentical;

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel" << false << "caseFirst"
                               << "off"
                               << "strength" << 5 << "numericOrdering" << false << "alternate"
                               << "non-ignorable"
                               << "maxVariable"
                               << "punct"
                               << "normalization" << false << "backwards" << false);

    ASSERT_EQ(expectedObj, CollationSerializer::specToBSON(collationSpec));
}

TEST(CollationSerializerTest, ToBSONCorrectlySerializesAlternateShifted) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";
    collationSpec.alternate = CollationSpec::AlternateType::kShifted;

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel" << false << "caseFirst"
                               << "off"
                               << "strength" << 3 << "numericOrdering" << false << "alternate"
                               << "shifted"
                               << "maxVariable"
                               << "punct"
                               << "normalization" << false << "backwards" << false);

    ASSERT_EQ(expectedObj, CollationSerializer::specToBSON(collationSpec));
}

TEST(CollationSerializerTest, ToBSONCorrectlySerializesMaxVariableSpace) {
    CollationSpec collationSpec;
    collationSpec.localeID = "myLocale";
    collationSpec.maxVariable = CollationSpec::MaxVariableType::kSpace;

    BSONObj expectedObj = BSON("locale"
                               << "myLocale"
                               << "caseLevel" << false << "caseFirst"
                               << "off"
                               << "strength" << 3 << "numericOrdering" << false << "alternate"
                               << "non-ignorable"
                               << "maxVariable"
                               << "space"
                               << "normalization" << false << "backwards" << false);

    ASSERT_EQ(expectedObj, CollationSerializer::specToBSON(collationSpec));
}

}  // namespace
