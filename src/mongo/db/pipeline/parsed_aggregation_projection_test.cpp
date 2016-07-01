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

#include "mongo/db/pipeline/parsed_aggregation_projection.h"

#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace parsed_aggregation_projection {
namespace {

template <typename T>
BSONObj wrapInLiteral(const T& arg) {
    return BSON("$literal" << arg);
}

//
// Error cases.
//

TEST(ParsedAggregationProjectionErrors, ShouldRejectDuplicateFieldNames) {
    // Include/exclude the same field twice.
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a" << true << "a" << true)),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a" << false << "a" << false)),
                  UserException);
    ASSERT_THROWS(
        ParsedAggregationProjection::create(BSON("a" << BSON("b" << false << "b" << false))),
        UserException);

    // Mix of include/exclude and adding a field.
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a" << wrapInLiteral(1) << "a" << true)),
                  UserException);
    ASSERT_THROWS(
        ParsedAggregationProjection::create(BSON("a" << false << "a" << wrapInLiteral(0))),
        UserException);

    // Adding the same field twice.
    ASSERT_THROWS(ParsedAggregationProjection::create(
                      BSON("a" << wrapInLiteral(1) << "a" << wrapInLiteral(0))),
                  UserException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectDuplicateIds) {
    // Include/exclude _id twice.
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("_id" << true << "_id" << true)),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("_id" << false << "_id" << false)),
                  UserException);

    // Mix of including/excluding and adding _id.
    ASSERT_THROWS(
        ParsedAggregationProjection::create(BSON("_id" << wrapInLiteral(1) << "_id" << true)),
        UserException);
    ASSERT_THROWS(
        ParsedAggregationProjection::create(BSON("_id" << false << "_id" << wrapInLiteral(0))),
        UserException);

    // Adding _id twice.
    ASSERT_THROWS(ParsedAggregationProjection::create(
                      BSON("_id" << wrapInLiteral(1) << "_id" << wrapInLiteral(0))),
                  UserException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectFieldsWithSharedPrefix) {
    // Include/exclude Fields with a shared prefix.
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a" << true << "a.b" << true)),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a.b" << false << "a" << false)),
                  UserException);

    // Mix of include/exclude and adding a shared prefix.
    ASSERT_THROWS(
        ParsedAggregationProjection::create(BSON("a" << wrapInLiteral(1) << "a.b" << true)),
        UserException);
    ASSERT_THROWS(
        ParsedAggregationProjection::create(BSON("a.b" << false << "a" << wrapInLiteral(0))),
        UserException);

    // Adding a shared prefix twice.
    ASSERT_THROWS(ParsedAggregationProjection::create(
                      BSON("a" << wrapInLiteral(1) << "a.b" << wrapInLiteral(0))),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(
                      BSON("a.b.c.d" << wrapInLiteral(1) << "a.b.c" << wrapInLiteral(0))),
                  UserException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectMixOfIdAndSubFieldsOfId) {
    // Include/exclude _id twice.
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("_id" << true << "_id.x" << true)),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("_id.x" << false << "_id" << false)),
                  UserException);

    // Mix of including/excluding and adding _id.
    ASSERT_THROWS(
        ParsedAggregationProjection::create(BSON("_id" << wrapInLiteral(1) << "_id.x" << true)),
        UserException);
    ASSERT_THROWS(
        ParsedAggregationProjection::create(BSON("_id.x" << false << "_id" << wrapInLiteral(0))),
        UserException);

    // Adding _id twice.
    ASSERT_THROWS(ParsedAggregationProjection::create(
                      BSON("_id" << wrapInLiteral(1) << "_id.x" << wrapInLiteral(0))),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(
                      BSON("_id.b.c.d" << wrapInLiteral(1) << "_id.b.c" << wrapInLiteral(0))),
                  UserException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectMixOfInclusionAndExclusion) {
    // Simple mix.
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a" << true << "b" << false)),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a" << false << "b" << true)),
                  UserException);
    ASSERT_THROWS(
        ParsedAggregationProjection::create(BSON("a" << BSON("b" << false << "c" << true))),
        UserException);
    ASSERT_THROWS(
        ParsedAggregationProjection::create(BSON("_id" << BSON("b" << false << "c" << true))),
        UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("_id.b" << false << "a.c" << true)),
                  UserException);

    // Mix while also adding a field.
    ASSERT_THROWS(ParsedAggregationProjection::create(
                      BSON("a" << true << "b" << wrapInLiteral(1) << "c" << false)),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(
                      BSON("a" << false << "b" << wrapInLiteral(1) << "c" << true)),
                  UserException);

    // Mixing "_id" inclusion with exclusion.
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("_id" << true << "a" << false)),
                  UserException);

    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a" << false << "_id" << true)),
                  UserException);

    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("_id" << true << "a.b.c" << false)),
                  UserException);

    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("_id.x" << true << "a.b.c" << false)),
                  UserException);
}

TEST(ParsedAggregationProjectionType, ShouldRejectMixOfExclusionAndComputedFields) {
    ASSERT_THROWS(
        ParsedAggregationProjection::create(BSON("a" << false << "b" << wrapInLiteral(1))),
        UserException);

    ASSERT_THROWS(
        ParsedAggregationProjection::create(BSON("a" << wrapInLiteral(1) << "b" << false)),
        UserException);

    ASSERT_THROWS(
        ParsedAggregationProjection::create(BSON("a.b" << false << "a.c" << wrapInLiteral(1))),
        UserException);

    ASSERT_THROWS(
        ParsedAggregationProjection::create(BSON("a.b" << wrapInLiteral(1) << "a.c" << false)),
        UserException);

    ASSERT_THROWS(ParsedAggregationProjection::create(
                      BSON("a" << BSON("b" << false << "c" << wrapInLiteral(1)))),
                  UserException);

    ASSERT_THROWS(ParsedAggregationProjection::create(
                      BSON("a" << BSON("b" << wrapInLiteral(1) << "c" << false))),
                  UserException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectDottedFieldInSubDocument) {
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a" << BSON("b.c" << true))),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a" << BSON("b.c" << wrapInLiteral(1)))),
                  UserException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectFieldNamesStartingWithADollar) {
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("$dollar" << 0)), UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("$dollar" << 1)), UserException);

    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("b.$dollar" << 0)), UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("b.$dollar" << 1)), UserException);

    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("b" << BSON("$dollar" << 0))),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("b" << BSON("$dollar" << 1))),
                  UserException);

    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("$add" << 0)), UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("$add" << 1)), UserException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectTopLevelExpressions) {
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("$add" << BSON_ARRAY(4 << 2))),
                  UserException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectExpressionWithMultipleFieldNames) {
    ASSERT_THROWS(ParsedAggregationProjection::create(
                      BSON("a" << BSON("$add" << BSON_ARRAY(4 << 2) << "b" << 1))),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(
                      BSON("a" << BSON("b" << 1 << "$add" << BSON_ARRAY(4 << 2)))),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(
                      BSON("a" << BSON("b" << BSON("c" << 1 << "$add" << BSON_ARRAY(4 << 2))))),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(
                      BSON("a" << BSON("b" << BSON("$add" << BSON_ARRAY(4 << 2) << "c" << 1)))),
                  UserException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectEmptyProjection) {
    ASSERT_THROWS(ParsedAggregationProjection::create(BSONObj()), UserException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectEmptyNestedObject) {
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a" << BSONObj())), UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a" << false << "b" << BSONObj())),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a" << true << "b" << BSONObj())),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a.b" << BSONObj())), UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a" << BSON("b" << BSONObj()))),
                  UserException);
}

TEST(ParsedAggregationProjectionErrors, ShouldErrorOnInvalidExpression) {
    ASSERT_THROWS(ParsedAggregationProjection::create(
                      BSON("a" << false << "b" << BSON("$unknown" << BSON_ARRAY(4 << 2)))),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(
                      BSON("a" << true << "b" << BSON("$unknown" << BSON_ARRAY(4 << 2)))),
                  UserException);
}

TEST(ParsedAggregationProjectionErrors, ShouldErrorOnInvalidFieldPath) {
    // Empty field names.
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("" << wrapInLiteral(2))), UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("" << true)), UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("" << false)), UserException);

    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a" << BSON("" << true))),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a" << BSON("" << false))),
                  UserException);

    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("" << BSON("a" << true))),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("" << BSON("a" << false))),
                  UserException);

    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a." << true)), UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("a." << false)), UserException);

    ASSERT_THROWS(ParsedAggregationProjection::create(BSON(".a" << true)), UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON(".a" << false)), UserException);

    // Not testing field names with null bytes, since that is invalid BSON, and won't make it to the
    // $project stage without a previous error.

    // Field names starting with '$'.
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("$x" << wrapInLiteral(2))),
                  UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("c.$d" << true)), UserException);
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("c.$d" << false)), UserException);
}

TEST(ParsedAggregationProjectionErrors, ShouldErrorOnProjectionWithNoOutputFields) {
    // This is treated as an inclusion projection without any fields, so should error.
    ASSERT_THROWS(ParsedAggregationProjection::create(BSON("_id" << false)), UserException);
}

TEST(ParsedAggregationProjectionErrors, ShouldNotErrorOnTwoNestedFields) {
    ParsedAggregationProjection::create(BSON("a.b" << true << "a.c" << true));
    ParsedAggregationProjection::create(BSON("a.b" << true << "a" << BSON("c" << true)));
}

//
// Determining exclusion vs. inclusion.
//

TEST(ParsedAggregationProjectionType, ShouldDefaultToInclusionProjection) {
    auto parsedProject = ParsedAggregationProjection::create(BSON("_id" << true));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);

    parsedProject = ParsedAggregationProjection::create(BSON("_id" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);

    parsedProject = ParsedAggregationProjection::create(BSON("a" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);
}

TEST(ParsedAggregationProjectionType, ShouldDetectExclusionProjection) {
    auto parsedProject = ParsedAggregationProjection::create(BSON("a" << false));
    ASSERT(parsedProject->getType() == ProjectionType::kExclusion);

    parsedProject = ParsedAggregationProjection::create(BSON("_id.x" << false));
    ASSERT(parsedProject->getType() == ProjectionType::kExclusion);

    parsedProject = ParsedAggregationProjection::create(BSON("_id" << BSON("x" << false)));
    ASSERT(parsedProject->getType() == ProjectionType::kExclusion);

    parsedProject = ParsedAggregationProjection::create(BSON("x" << BSON("_id" << false)));
    ASSERT(parsedProject->getType() == ProjectionType::kExclusion);
}

TEST(ParsedAggregationProjectionType, ShouldDetectInclusionProjection) {
    auto parsedProject = ParsedAggregationProjection::create(BSON("a" << true));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);

    parsedProject = ParsedAggregationProjection::create(BSON("_id" << false << "a" << true));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);

    parsedProject = ParsedAggregationProjection::create(BSON("_id" << false << "a.b.c" << true));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);

    parsedProject = ParsedAggregationProjection::create(BSON("_id.x" << true));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);

    parsedProject = ParsedAggregationProjection::create(BSON("_id" << BSON("x" << true)));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);

    parsedProject = ParsedAggregationProjection::create(BSON("x" << BSON("_id" << true)));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);
}

TEST(ParsedAggregationProjectionType, ShouldTreatOnlyComputedFieldsAsAnInclusionProjection) {
    auto parsedProject = ParsedAggregationProjection::create(BSON("a" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);

    parsedProject =
        ParsedAggregationProjection::create(BSON("_id" << false << "a" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);

    parsedProject =
        ParsedAggregationProjection::create(BSON("_id" << false << "a.b.c" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);

    parsedProject = ParsedAggregationProjection::create(BSON("_id.x" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);

    parsedProject =
        ParsedAggregationProjection::create(BSON("_id" << BSON("x" << wrapInLiteral(1))));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);

    parsedProject =
        ParsedAggregationProjection::create(BSON("x" << BSON("_id" << wrapInLiteral(1))));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);
}

TEST(ParsedAggregationProjectionType, ShouldAllowMixOfInclusionAndComputedFields) {
    auto parsedProject =
        ParsedAggregationProjection::create(BSON("a" << true << "b" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);

    parsedProject =
        ParsedAggregationProjection::create(BSON("a.b" << true << "a.c" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);

    parsedProject = ParsedAggregationProjection::create(
        BSON("a" << BSON("b" << true << "c" << wrapInLiteral(1))));
    ASSERT(parsedProject->getType() == ProjectionType::kInclusion);
}

TEST(ParsedAggregationProjectionType, ShouldCoerceNumericsToBools) {
    std::vector<Value> zeros = {Value(0), Value(0LL), Value(0.0), Value(Decimal128(0))};
    for (auto&& zero : zeros) {
        auto parsedProject = ParsedAggregationProjection::create(Document{{"a", zero}}.toBson());
        ASSERT(parsedProject->getType() == ProjectionType::kExclusion);
    }

    std::vector<Value> nonZeroes = {
        Value(1), Value(-1), Value(3), Value(1LL), Value(1.0), Value(Decimal128(1))};
    for (auto&& nonZero : nonZeroes) {
        auto parsedProject = ParsedAggregationProjection::create(Document{{"a", nonZero}}.toBson());
        ASSERT(parsedProject->getType() == ProjectionType::kInclusion);
    }
}

}  // namespace
}  // namespace parsed_aggregation_projection
}  // namespace mongo
