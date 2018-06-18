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
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace parsed_aggregation_projection {
namespace {

using ProjectionArrayRecursionPolicy = ParsedAggregationProjection::ProjectionArrayRecursionPolicy;
using ProjectionDefaultIdPolicy = ParsedAggregationProjection::ProjectionDefaultIdPolicy;
using ProjectionParseMode = ParsedAggregationProjection::ProjectionParseMode;

template <typename T>
BSONObj wrapInLiteral(const T& arg) {
    return BSON("$literal" << arg);
}

// Helper to simplify the creation of a ParsedAggregationProjection which includes _id and recurses
// nested arrays by default.
std::unique_ptr<ParsedAggregationProjection> makeProjectionWithDefaultPolicies(BSONObj projSpec) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    return ParsedAggregationProjection::create(
        expCtx,
        projSpec,
        ProjectionDefaultIdPolicy::kIncludeId,
        ProjectionArrayRecursionPolicy::kRecurseNestedArrays);
}

//
// Error cases.
//

TEST(ParsedAggregationProjectionErrors, ShouldRejectDuplicateFieldNames) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // Include/exclude the same field twice.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << true << "a" << true)),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << false << "a" << false)),
                  AssertionException);
    ASSERT_THROWS(
        makeProjectionWithDefaultPolicies(BSON("a" << BSON("b" << false << "b" << false))),
        AssertionException);

    // Mix of include/exclude and adding a field.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << wrapInLiteral(1) << "a" << true)),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << false << "a" << wrapInLiteral(0))),
                  AssertionException);

    // Adding the same field twice.
    ASSERT_THROWS(
        makeProjectionWithDefaultPolicies(BSON("a" << wrapInLiteral(1) << "a" << wrapInLiteral(0))),
        AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectDuplicateIds) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // Include/exclude _id twice.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("_id" << true << "_id" << true)),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("_id" << false << "_id" << false)),
                  AssertionException);

    // Mix of including/excluding and adding _id.
    ASSERT_THROWS(
        makeProjectionWithDefaultPolicies(BSON("_id" << wrapInLiteral(1) << "_id" << true)),
        AssertionException);
    ASSERT_THROWS(
        makeProjectionWithDefaultPolicies(BSON("_id" << false << "_id" << wrapInLiteral(0))),
        AssertionException);

    // Adding _id twice.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("_id" << wrapInLiteral(1) << "_id" << wrapInLiteral(0))),
                  AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectFieldsWithSharedPrefix) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // Include/exclude Fields with a shared prefix.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << true << "a.b" << true)),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a.b" << false << "a" << false)),
                  AssertionException);

    // Mix of include/exclude and adding a shared prefix.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << wrapInLiteral(1) << "a.b" << true)),
                  AssertionException);
    ASSERT_THROWS(
        makeProjectionWithDefaultPolicies(BSON("a.b" << false << "a" << wrapInLiteral(0))),
        AssertionException);

    // Adding a shared prefix twice.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a" << wrapInLiteral(1) << "a.b" << wrapInLiteral(0))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a.b.c.d" << wrapInLiteral(1) << "a.b.c" << wrapInLiteral(0))),
                  AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectPathConflictsWithNonAlphaNumericCharacters) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // Include/exclude non-alphanumeric fields with a shared prefix. First assert that the non-
    // alphanumeric fields are accepted when no prefixes are present.
    ASSERT(makeProjectionWithDefaultPolicies(
        BSON("a.b-c" << true << "a.b" << true << "a.b?c" << true << "a.b c" << true)));
    ASSERT(makeProjectionWithDefaultPolicies(
        BSON("a.b c" << false << "a.b?c" << false << "a.b" << false << "a.b-c" << false)));

    // Then assert that we throw when we introduce a prefixed field.
    ASSERT_THROWS(
        makeProjectionWithDefaultPolicies(
            BSON("a.b-c" << true << "a.b" << true << "a.b?c" << true << "a.b c" << true << "a.b.d"
                         << true)),
        AssertionException);
    ASSERT_THROWS(
        makeProjectionWithDefaultPolicies(BSON(
            "a.b.d" << false << "a.b c" << false << "a.b?c" << false << "a.b" << false << "a.b-c"
                    << false)),
        AssertionException);

    // Adding the same field twice.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a.b?c" << wrapInLiteral(1) << "a.b?c" << wrapInLiteral(0))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a.b c" << wrapInLiteral(0) << "a.b c" << wrapInLiteral(1))),
                  AssertionException);

    // Mix of include/exclude and adding a shared prefix.
    ASSERT_THROWS(
        makeProjectionWithDefaultPolicies(
            BSON("a.b-c" << true << "a.b" << wrapInLiteral(1) << "a.b?c" << true << "a.b c" << true
                         << "a.b.d"
                         << true)),
        AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a.b.d" << false << "a.b c" << false << "a.b?c" << false << "a.b"
                                   << wrapInLiteral(0)
                                   << "a.b-c"
                                   << false)),
                  AssertionException);

    // Adding a shared prefix twice.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a.b-c" << wrapInLiteral(1) << "a.b" << wrapInLiteral(1) << "a.b?c"
                                   << wrapInLiteral(1)
                                   << "a.b c"
                                   << wrapInLiteral(1)
                                   << "a.b.d"
                                   << wrapInLiteral(0))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a.b.d" << wrapInLiteral(1) << "a.b c" << wrapInLiteral(1) << "a.b?c"
                                   << wrapInLiteral(1)
                                   << "a.b"
                                   << wrapInLiteral(0)
                                   << "a.b-c"
                                   << wrapInLiteral(1))),
                  AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectMixOfIdAndSubFieldsOfId) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // Include/exclude _id twice.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("_id" << true << "_id.x" << true)),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("_id.x" << false << "_id" << false)),
                  AssertionException);

    // Mix of including/excluding and adding _id.
    ASSERT_THROWS(
        makeProjectionWithDefaultPolicies(BSON("_id" << wrapInLiteral(1) << "_id.x" << true)),
        AssertionException);
    ASSERT_THROWS(
        makeProjectionWithDefaultPolicies(BSON("_id.x" << false << "_id" << wrapInLiteral(0))),
        AssertionException);

    // Adding _id twice.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("_id" << wrapInLiteral(1) << "_id.x" << wrapInLiteral(0))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("_id.b.c.d" << wrapInLiteral(1) << "_id.b.c" << wrapInLiteral(0))),
                  AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldAllowMixOfIdInclusionAndExclusion) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    // Mixing "_id" inclusion with exclusion.
    auto parsedProject = makeProjectionWithDefaultPolicies(BSON("_id" << true << "a" << false));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("a" << false << "_id" << true));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("_id" << true << "a.b.c" << false));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectMixOfInclusionAndExclusion) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // Simple mix.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << true << "b" << false)),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << false << "b" << true)),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << BSON("b" << false << "c" << true))),
                  AssertionException);
    ASSERT_THROWS(
        makeProjectionWithDefaultPolicies(BSON("_id" << BSON("b" << false << "c" << true))),
        AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("_id.b" << false << "a.c" << true)),
                  AssertionException);

    // Mix while also adding a field.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a" << true << "b" << wrapInLiteral(1) << "c" << false)),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a" << false << "b" << wrapInLiteral(1) << "c" << true)),
                  AssertionException);

    // Mix of "_id" subfield inclusion and exclusion.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("_id.x" << true << "a.b.c" << false)),
                  AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectMixOfExclusionAndComputedFields) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << false << "b" << wrapInLiteral(1))),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << wrapInLiteral(1) << "b" << false)),
                  AssertionException);

    ASSERT_THROWS(
        makeProjectionWithDefaultPolicies(BSON("a.b" << false << "a.c" << wrapInLiteral(1))),
        AssertionException);

    ASSERT_THROWS(
        makeProjectionWithDefaultPolicies(BSON("a.b" << wrapInLiteral(1) << "a.c" << false)),
        AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a" << BSON("b" << false << "c" << wrapInLiteral(1)))),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a" << BSON("b" << wrapInLiteral(1) << "c" << false))),
                  AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectDottedFieldInSubDocument) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << BSON("b.c" << true))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << BSON("b.c" << wrapInLiteral(1)))),
                  AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectFieldNamesStartingWithADollar) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("$dollar" << 0)), AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("$dollar" << 1)), AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("b.$dollar" << 0)), AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("b.$dollar" << 1)), AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("b" << BSON("$dollar" << 0))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("b" << BSON("$dollar" << 1))),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("$add" << 0)), AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("$add" << 1)), AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectTopLevelExpressions) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("$add" << BSON_ARRAY(4 << 2))),
                  AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectExpressionWithMultipleFieldNames) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a" << BSON("$add" << BSON_ARRAY(4 << 2) << "b" << 1))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a" << BSON("b" << 1 << "$add" << BSON_ARRAY(4 << 2)))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a" << BSON("b" << BSON("c" << 1 << "$add" << BSON_ARRAY(4 << 2))))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a" << BSON("b" << BSON("$add" << BSON_ARRAY(4 << 2) << "c" << 1)))),
                  AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectEmptyProjection) {
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSONObj()), AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectEmptyNestedObject) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << BSONObj())), AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << false << "b" << BSONObj())),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << true << "b" << BSONObj())),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a.b" << BSONObj())), AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << BSON("b" << BSONObj()))),
                  AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldErrorOnInvalidExpression) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a" << false << "b" << BSON("$unknown" << BSON_ARRAY(4 << 2)))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a" << true << "b" << BSON("$unknown" << BSON_ARRAY(4 << 2)))),
                  AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldErrorOnInvalidFieldPath) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // Empty field names.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("" << wrapInLiteral(2))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("" << true)), AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("" << false)), AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << BSON("" << true))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << BSON("" << false))),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("" << BSON("a" << true))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("" << BSON("a" << false))),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a." << true)), AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a." << false)), AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON(".a" << true)), AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON(".a" << false)), AssertionException);

    // Not testing field names with null bytes, since that is invalid BSON, and won't make it to the
    // $project stage without a previous error.

    // Field names starting with '$'.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("$x" << wrapInLiteral(2))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("c.$d" << true)), AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("c.$d" << false)), AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldNotErrorOnTwoNestedFields) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    makeProjectionWithDefaultPolicies(BSON("a.b" << true << "a.c" << true));
    makeProjectionWithDefaultPolicies(BSON("a.b" << true << "a" << BSON("c" << true)));
}

//
// Determining exclusion vs. inclusion.
//

TEST(ParsedAggregationProjectionType, ShouldDefaultToInclusionProjection) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto parsedProject = makeProjectionWithDefaultPolicies(BSON("_id" << true));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("_id" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("a" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);
}

TEST(ParsedAggregationProjectionType, ShouldDetectExclusionProjection) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto parsedProject = makeProjectionWithDefaultPolicies(BSON("a" << false));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("_id.x" << false));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("_id" << BSON("x" << false)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("x" << BSON("_id" << false)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("_id" << false));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);
}

TEST(ParsedAggregationProjectionType, ShouldDetectInclusionProjection) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto parsedProject = makeProjectionWithDefaultPolicies(BSON("a" << true));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("_id" << false << "a" << true));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("_id" << false << "a.b.c" << true));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("_id.x" << true));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("_id" << BSON("x" << true)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("x" << BSON("_id" << true)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);
}

TEST(ParsedAggregationProjectionType, ShouldTreatOnlyComputedFieldsAsAnInclusionProjection) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto parsedProject = makeProjectionWithDefaultPolicies(BSON("a" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject =
        makeProjectionWithDefaultPolicies(BSON("_id" << false << "a" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject =
        makeProjectionWithDefaultPolicies(BSON("_id" << false << "a.b.c" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("_id.x" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("_id" << BSON("x" << wrapInLiteral(1))));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("x" << BSON("_id" << wrapInLiteral(1))));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);
}

TEST(ParsedAggregationProjectionType, ShouldAllowMixOfInclusionAndComputedFields) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto parsedProject =
        makeProjectionWithDefaultPolicies(BSON("a" << true << "b" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject =
        makeProjectionWithDefaultPolicies(BSON("a.b" << true << "a.c" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(
        BSON("a" << BSON("b" << true << "c" << wrapInLiteral(1))));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("a" << BSON("b" << true << "c"
                                                                           << "stringLiteral")));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);
}

TEST(ParsedAggregationProjectionType, ShouldRejectMixOfInclusionAndBannedComputedFields) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_THROWS(
        ParsedAggregationProjection::create(expCtx,
                                            BSON("a" << true << "b" << wrapInLiteral(1)),
                                            ProjectionDefaultIdPolicy::kIncludeId,
                                            ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                                            ProjectionParseMode::kBanComputedFields),
        AssertionException);

    ASSERT_THROWS(
        ParsedAggregationProjection::create(expCtx,
                                            BSON("a.b" << true << "a.c" << wrapInLiteral(1)),
                                            ProjectionDefaultIdPolicy::kIncludeId,
                                            ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                                            ProjectionParseMode::kBanComputedFields),
        AssertionException);

    ASSERT_THROWS(ParsedAggregationProjection::create(
                      expCtx,
                      BSON("a" << BSON("b" << true << "c" << wrapInLiteral(1))),
                      ProjectionDefaultIdPolicy::kIncludeId,
                      ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                      ProjectionParseMode::kBanComputedFields),
                  AssertionException);

    ASSERT_THROWS(
        ParsedAggregationProjection::create(expCtx,
                                            BSON("a" << BSON("b" << true << "c"
                                                                 << "stringLiteral")),
                                            ProjectionDefaultIdPolicy::kIncludeId,
                                            ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                                            ProjectionParseMode::kBanComputedFields),
        AssertionException);
}

TEST(ParsedAggregationProjectionType, ShouldRejectOnlyComputedFieldsWhenComputedFieldsAreBanned) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_THROWS(ParsedAggregationProjection::create(
                      expCtx,
                      BSON("a" << wrapInLiteral(1) << "b" << wrapInLiteral(2)),
                      ProjectionDefaultIdPolicy::kIncludeId,
                      ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                      ProjectionParseMode::kBanComputedFields),
                  AssertionException);

    ASSERT_THROWS(ParsedAggregationProjection::create(
                      expCtx,
                      BSON("a.b" << wrapInLiteral(1) << "a.c" << wrapInLiteral(2)),
                      ProjectionDefaultIdPolicy::kIncludeId,
                      ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                      ProjectionParseMode::kBanComputedFields),
                  AssertionException);

    ASSERT_THROWS(ParsedAggregationProjection::create(
                      expCtx,
                      BSON("a" << BSON("b" << wrapInLiteral(1) << "c" << wrapInLiteral(2))),
                      ProjectionDefaultIdPolicy::kIncludeId,
                      ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                      ProjectionParseMode::kBanComputedFields),
                  AssertionException);

    ASSERT_THROWS(ParsedAggregationProjection::create(
                      expCtx,
                      BSON("a" << BSON("b" << wrapInLiteral(1) << "c" << wrapInLiteral(2))),
                      ProjectionDefaultIdPolicy::kIncludeId,
                      ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                      ProjectionParseMode::kBanComputedFields),
                  AssertionException);
}

TEST(ParsedAggregationProjectionType, ShouldAcceptInclusionProjectionWhenComputedFieldsAreBanned) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto parsedProject =
        ParsedAggregationProjection::create(expCtx,
                                            BSON("a" << true),
                                            ProjectionDefaultIdPolicy::kIncludeId,
                                            ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                                            ProjectionParseMode::kBanComputedFields);
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject =
        ParsedAggregationProjection::create(expCtx,
                                            BSON("_id" << false << "a" << true),
                                            ProjectionDefaultIdPolicy::kIncludeId,
                                            ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                                            ProjectionParseMode::kBanComputedFields);
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject =
        ParsedAggregationProjection::create(expCtx,
                                            BSON("_id" << false << "a.b.c" << true),
                                            ProjectionDefaultIdPolicy::kIncludeId,
                                            ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                                            ProjectionParseMode::kBanComputedFields);
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject =
        ParsedAggregationProjection::create(expCtx,
                                            BSON("_id.x" << true),
                                            ProjectionDefaultIdPolicy::kIncludeId,
                                            ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                                            ProjectionParseMode::kBanComputedFields);
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject =
        ParsedAggregationProjection::create(expCtx,
                                            BSON("_id" << BSON("x" << true)),
                                            ProjectionDefaultIdPolicy::kIncludeId,
                                            ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                                            ProjectionParseMode::kBanComputedFields);
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject =
        ParsedAggregationProjection::create(expCtx,
                                            BSON("x" << BSON("_id" << true)),
                                            ProjectionDefaultIdPolicy::kIncludeId,
                                            ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                                            ProjectionParseMode::kBanComputedFields);
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);
}

TEST(ParsedAggregationProjectionType, ShouldAcceptExclusionProjectionWhenComputedFieldsAreBanned) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto parsedProject =
        ParsedAggregationProjection::create(expCtx,
                                            BSON("a" << false),
                                            ProjectionDefaultIdPolicy::kIncludeId,
                                            ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                                            ProjectionParseMode::kBanComputedFields);
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject =
        ParsedAggregationProjection::create(expCtx,
                                            BSON("_id.x" << false),
                                            ProjectionDefaultIdPolicy::kIncludeId,
                                            ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                                            ProjectionParseMode::kBanComputedFields);
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject =
        ParsedAggregationProjection::create(expCtx,
                                            BSON("_id" << BSON("x" << false)),
                                            ProjectionDefaultIdPolicy::kIncludeId,
                                            ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                                            ProjectionParseMode::kBanComputedFields);
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject =
        ParsedAggregationProjection::create(expCtx,
                                            BSON("x" << BSON("_id" << false)),
                                            ProjectionDefaultIdPolicy::kIncludeId,
                                            ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                                            ProjectionParseMode::kBanComputedFields);
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject =
        ParsedAggregationProjection::create(expCtx,
                                            BSON("_id" << false),
                                            ProjectionDefaultIdPolicy::kIncludeId,
                                            ProjectionArrayRecursionPolicy::kRecurseNestedArrays,
                                            ProjectionParseMode::kBanComputedFields);
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);
}

TEST(ParsedAggregationProjectionType, ShouldCoerceNumericsToBools) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::vector<Value> zeros = {Value(0), Value(0LL), Value(0.0), Value(Decimal128(0))};
    for (auto&& zero : zeros) {
        auto parsedProject = makeProjectionWithDefaultPolicies(Document{{"a", zero}}.toBson());
        ASSERT(parsedProject->getType() ==
               TransformerInterface::TransformerType::kExclusionProjection);
    }

    std::vector<Value> nonZeroes = {
        Value(1), Value(-1), Value(3), Value(1LL), Value(1.0), Value(Decimal128(1))};
    for (auto&& nonZero : nonZeroes) {
        auto parsedProject = makeProjectionWithDefaultPolicies(Document{{"a", nonZero}}.toBson());
        ASSERT(parsedProject->getType() ==
               TransformerInterface::TransformerType::kInclusionProjection);
    }
}

}  // namespace
}  // namespace parsed_aggregation_projection
}  // namespace mongo
