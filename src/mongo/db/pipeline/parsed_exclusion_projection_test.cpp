/**
 *    Copyright (C) 2016 MongoDB, Inc.
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

#include "mongo/db/pipeline/parsed_exclusion_projection.h"

#include <iostream>
#include <iterator>
#include <string>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace parsed_aggregation_projection {
namespace {
using std::vector;

//
// Errors.
//

DEATH_TEST(ExclusionProjection,
           ShouldRejectComputedField,
           "Invariant failure fieldName[0] != '$'") {
    ParsedExclusionProjection exclusion;
    // Top-level expression.
    exclusion.parse(BSON("a" << false << "b" << BSON("$literal" << 1)));
}

DEATH_TEST(ExclusionProjection,
           ShouldFailWhenGivenIncludedField,
           "Invariant failure !elem.trueValue()") {
    ParsedExclusionProjection exclusion;
    exclusion.parse(BSON("a" << true));
}

DEATH_TEST(ExclusionProjection,
           ShouldFailWhenGivenIncludedId,
           "Invariant failure !elem.trueValue()") {
    ParsedExclusionProjection exclusion;
    exclusion.parse(BSON("_id" << true << "a" << false));
}

TEST(ExclusionProjection, ShouldSerializeToEquivalentProjection) {
    ParsedExclusionProjection exclusion;
    exclusion.parse(
        fromjson("{a: 0, b: {c: NumberLong(0), d: 0.0}, 'x.y': false, _id: NumberInt(0)}"));

    // Converts numbers to bools, converts dotted paths to nested documents. Note order of excluded
    // fields is subject to change.
    auto serialization = exclusion.serialize();
    ASSERT_EQ(serialization.size(), 4UL);
    ASSERT_VALUE_EQ(serialization["a"], Value(false));
    ASSERT_VALUE_EQ(serialization["_id"], Value(false));

    ASSERT_EQ(serialization["b"].getType(), BSONType::Object);
    ASSERT_EQ(serialization["b"].getDocument().size(), 2UL);
    ASSERT_VALUE_EQ(serialization["b"].getDocument()["c"], Value(false));
    ASSERT_VALUE_EQ(serialization["b"].getDocument()["d"], Value(false));

    ASSERT_EQ(serialization["x"].getType(), BSONType::Object);
    ASSERT_EQ(serialization["x"].getDocument().size(), 1UL);
    ASSERT_VALUE_EQ(serialization["x"].getDocument()["y"], Value(false));
}

TEST(ExclusionProjection, ShouldNotAddAnyDependencies) {
    // An exclusion projection will cause the $project stage to return GetDepsReturn::SEE_NEXT,
    // meaning it doesn't strictly require any fields.
    //
    // For example, if our projection was {a: 0}, and a later stage requires the field "a", then "a"
    // will be added to the dependencies correctly. If a later stage doesn't need "a", then we don't
    // need to include the "a" in the dependencies of this projection, since it will just be ignored
    // later. If there are no later stages, then we will finish the dependency computation
    // cycle without full knowledge of which fields are needed, and thus include all the fields.
    ParsedExclusionProjection exclusion;
    exclusion.parse(BSON("_id" << false << "a" << false << "b.c" << false << "x.y.z" << false));

    DepsTracker deps;
    exclusion.addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 0UL);
    ASSERT_FALSE(deps.needWholeDocument);
    ASSERT_FALSE(deps.getNeedTextScore());
}

//
// Tests of execution of exclusions at the top level.
//

TEST(ExclusionProjectionExecutionTest, ShouldExcludeTopLevelField) {
    ParsedExclusionProjection exclusion;
    exclusion.parse(BSON("a" << false));

    // More than one field in document.
    auto result = exclusion.applyProjection(Document{{"a", 1}, {"b", 2}});
    auto expectedResult = Document{{"b", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is the only field in the document.
    result = exclusion.applyProjection(Document{{"a", 1}});
    expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is not present in the document.
    result = exclusion.applyProjection(Document{{"c", 1}});
    expectedResult = Document{{"c", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are no fields in the document.
    result = exclusion.applyProjection(Document{});
    expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldCoerceNumericsToBools) {
    ParsedExclusionProjection exclusion;
    exclusion.parse(BSON("a" << Value(0) << "b" << Value(0LL) << "c" << Value(0.0) << "d"
                             << Value(Decimal128(0))));

    auto result = exclusion.applyProjection(Document{{"_id", "ID"}, {"a", 1}, {"b", 2}, {"c", 3}});
    auto expectedResult = Document{{"_id", "ID"}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldPreserveOrderOfExistingFields) {
    ParsedExclusionProjection exclusion;
    exclusion.parse(BSON("second" << false));
    auto result = exclusion.applyProjection(Document{{"first", 0}, {"second", 1}, {"third", 2}});
    auto expectedResult = Document{{"first", 0}, {"third", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldImplicitlyIncludeId) {
    ParsedExclusionProjection exclusion;
    exclusion.parse(BSON("a" << false));
    auto result = exclusion.applyProjection(Document{{"a", 1}, {"b", 2}, {"_id", "ID"}});
    auto expectedResult = Document{{"b", 2}, {"_id", "ID"}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldExcludeIdIfExplicitlyExcluded) {
    ParsedExclusionProjection exclusion;
    exclusion.parse(BSON("a" << false << "_id" << false));
    auto result = exclusion.applyProjection(Document{{"a", 1}, {"b", 2}, {"_id", "ID"}});
    auto expectedResult = Document{{"b", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

//
// Tests of execution of nested exclusions.
//

TEST(ExclusionProjectionExecutionTest, ShouldExcludeSubFieldsOfId) {
    ParsedExclusionProjection exclusion;
    exclusion.parse(BSON("_id.x" << false << "_id" << BSON("y" << false)));
    auto result = exclusion.applyProjection(
        Document{{"_id", Document{{"x", 1}, {"y", 2}, {"z", 3}}}, {"a", 1}});
    auto expectedResult = Document{{"_id", Document{{"z", 3}}}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldExcludeSimpleDottedFieldFromSubDoc) {
    ParsedExclusionProjection exclusion;
    exclusion.parse(BSON("a.b" << false));

    // More than one field in sub document.
    auto result = exclusion.applyProjection(Document{{"a", Document{{"b", 1}, {"c", 2}}}});
    auto expectedResult = Document{{"a", Document{{"c", 2}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is the only field in the sub document.
    result = exclusion.applyProjection(Document{{"a", Document{{"b", 1}}}});
    expectedResult = Document{{"a", Document{}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is not present in the sub document.
    result = exclusion.applyProjection(Document{{"a", Document{{"c", 1}}}});
    expectedResult = Document{{"a", Document{{"c", 1}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are no fields in sub document.
    result = exclusion.applyProjection(Document{{"a", Document{}}});
    expectedResult = Document{{"a", Document{}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldNotCreateSubDocIfDottedExcludedFieldDoesNotExist) {
    ParsedExclusionProjection exclusion;
    exclusion.parse(BSON("sub.target" << false));

    // Should not add the path if it doesn't exist.
    auto result = exclusion.applyProjection(Document{});
    auto expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Should not replace non-documents with documents.
    result = exclusion.applyProjection(Document{{"sub", "notADocument"}});
    expectedResult = Document{{"sub", "notADocument"}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldApplyDottedExclusionToEachElementInArray) {
    ParsedExclusionProjection exclusion;
    exclusion.parse(BSON("a.b" << false));

    std::vector<Value> nestedValues = {
        Value(1),
        Value(Document{}),
        Value(Document{{"b", 1}}),
        Value(Document{{"b", 1}, {"c", 2}}),
        Value(vector<Value>{}),
        Value(vector<Value>{Value(1), Value(Document{{"c", 1}, {"b", 1}})})};
    std::vector<Value> expectedNestedValues = {
        Value(1),
        Value(Document{}),
        Value(Document{}),
        Value(Document{{"c", 2}}),
        Value(vector<Value>{}),
        Value(vector<Value>{Value(1), Value(Document{{"c", 1}})})};
    auto result = exclusion.applyProjection(Document{{"a", nestedValues}});
    auto expectedResult = Document{{"a", expectedNestedValues}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldAllowMixedNestedAndDottedFields) {
    ParsedExclusionProjection exclusion;
    // Exclude all of "a.b", "a.c", "a.d", and "a.e".
    exclusion.parse(
        BSON("a.b" << false << "a.c" << false << "a" << BSON("d" << false << "e" << false)));
    auto result = exclusion.applyProjection(
        Document{{"a", Document{{"b", 1}, {"c", 2}, {"d", 3}, {"e", 4}, {"f", 5}}}});
    auto expectedResult = Document{{"a", Document{{"f", 5}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldAlwaysKeepMetadataFromOriginalDoc) {
    ParsedExclusionProjection exclusion;
    exclusion.parse(BSON("a" << false));

    MutableDocument inputDocBuilder(Document{{"_id", "ID"}, {"a", 1}});
    inputDocBuilder.setRandMetaField(1.0);
    inputDocBuilder.setTextScore(10.0);
    Document inputDoc = inputDocBuilder.freeze();

    auto result = exclusion.applyProjection(inputDoc);

    MutableDocument expectedDoc(Document{{"_id", "ID"}});
    expectedDoc.copyMetaDataFrom(inputDoc);
    ASSERT_DOCUMENT_EQ(result, expectedDoc.freeze());
}

}  // namespace
}  // namespace parsed_aggregation_projection
}  // namespace mongo
