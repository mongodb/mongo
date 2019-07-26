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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/parsed_aggregation_projection.h"

#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/parsed_inclusion_projection.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace parsed_aggregation_projection {
namespace {

using ProjectionPolicies = ParsedAggregationProjection::ProjectionPolicies;

template <typename T>
BSONObj wrapInLiteral(const T& arg) {
    return BSON("$literal" << arg);
}

// Helper to simplify the creation of a ParsedAggregationProjection with default policies.
std::unique_ptr<ParsedAggregationProjection> makeProjectionWithDefaultPolicies(BSONObj spec) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ParsedAggregationProjection::ProjectionPolicies defaultPolicies;
    return ParsedAggregationProjection::create(expCtx, spec, defaultPolicies);
}

// Helper to simplify the creation of a ParsedAggregationProjection which bans computed fields.
std::unique_ptr<ParsedAggregationProjection> makeProjectionWithBannedComputedFields(BSONObj spec) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ParsedAggregationProjection::ProjectionPolicies banComputedFields;
    banComputedFields.computedFieldsPolicy =
        ProjectionPolicies::ComputedFieldsPolicy::kBanComputedFields;
    return ParsedAggregationProjection::create(expCtx, spec, banComputedFields);
}

//
// Error cases.
//

TEST(ParsedAggregationProjectionErrors, ShouldRejectDuplicateFieldNames) {
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
    // Include/exclude non-alphanumeric fields with a shared prefix. First assert that the non-
    // alphanumeric fields are accepted when no prefixes are present.
    ASSERT(makeProjectionWithDefaultPolicies(
        BSON("a.b-c" << true << "a.b" << true << "a.b?c" << true << "a.b c" << true)));
    ASSERT(makeProjectionWithDefaultPolicies(
        BSON("a.b c" << false << "a.b?c" << false << "a.b" << false << "a.b-c" << false)));

    // Then assert that we throw when we introduce a prefixed field.
    ASSERT_THROWS(
        makeProjectionWithDefaultPolicies(BSON("a.b-c" << true << "a.b" << true << "a.b?c" << true
                                                       << "a.b c" << true << "a.b.d" << true)),
        AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a.b.d" << false << "a.b c" << false
                                                                 << "a.b?c" << false << "a.b"
                                                                 << false << "a.b-c" << false)),
                  AssertionException);

    // Adding the same field twice.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a.b?c" << wrapInLiteral(1) << "a.b?c" << wrapInLiteral(0))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a.b c" << wrapInLiteral(0) << "a.b c" << wrapInLiteral(1))),
                  AssertionException);

    // Mix of include/exclude and adding a shared prefix.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a.b-c" << true << "a.b" << wrapInLiteral(1) << "a.b?c" << true
                                   << "a.b c" << true << "a.b.d" << true)),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a.b.d" << false << "a.b c" << false << "a.b?c" << false << "a.b"
                                   << wrapInLiteral(0) << "a.b-c" << false)),
                  AssertionException);

    // Adding a shared prefix twice.
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a.b-c" << wrapInLiteral(1) << "a.b" << wrapInLiteral(1) << "a.b?c"
                                   << wrapInLiteral(1) << "a.b c" << wrapInLiteral(1) << "a.b.d"
                                   << wrapInLiteral(0))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a.b.d" << wrapInLiteral(1) << "a.b c" << wrapInLiteral(1) << "a.b?c"
                                   << wrapInLiteral(1) << "a.b" << wrapInLiteral(0) << "a.b-c"
                                   << wrapInLiteral(1))),
                  AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectMixOfIdAndSubFieldsOfId) {
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
    // Mixing "_id" inclusion with exclusion.
    auto parsedProject = makeProjectionWithDefaultPolicies(BSON("_id" << true << "a" << false));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("a" << false << "_id" << true));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("_id" << true << "a.b.c" << false));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectMixOfInclusionAndExclusion) {
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
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << BSON("b.c" << true))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("a" << BSON("b.c" << wrapInLiteral(1)))),
                  AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectFieldNamesStartingWithADollar) {
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
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(BSON("$add" << BSON_ARRAY(4 << 2))),
                  AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldRejectExpressionWithMultipleFieldNames) {
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
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a" << false << "b" << BSON("$unknown" << BSON_ARRAY(4 << 2)))),
                  AssertionException);
    ASSERT_THROWS(makeProjectionWithDefaultPolicies(
                      BSON("a" << true << "b" << BSON("$unknown" << BSON_ARRAY(4 << 2)))),
                  AssertionException);
}

TEST(ParsedAggregationProjectionErrors, ShouldErrorOnInvalidFieldPath) {
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
    makeProjectionWithDefaultPolicies(BSON("a.b" << true << "a.c" << true));
    makeProjectionWithDefaultPolicies(BSON("a.b" << true << "a" << BSON("c" << true)));
}

//
// Determining exclusion vs. inclusion.
//

TEST(ParsedAggregationProjectionType, ShouldDefaultToInclusionProjection) {
    auto parsedProject = makeProjectionWithDefaultPolicies(BSON("_id" << true));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("_id" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultPolicies(BSON("a" << wrapInLiteral(1)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);
}

TEST(ParsedAggregationProjectionType, ShouldDetectExclusionProjection) {
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
    ASSERT_THROWS(
        makeProjectionWithBannedComputedFields(BSON("a" << true << "b" << wrapInLiteral(1))),
        AssertionException);

    ASSERT_THROWS(
        makeProjectionWithBannedComputedFields(BSON("a.b" << true << "a.c" << wrapInLiteral(1))),
        AssertionException);

    ASSERT_THROWS(makeProjectionWithBannedComputedFields(
                      BSON("a" << BSON("b" << true << "c" << wrapInLiteral(1)))),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithBannedComputedFields(BSON("a" << BSON("b" << true << "c"
                                                                              << "stringLiteral"))),
                  AssertionException);
}

TEST(ParsedAggregationProjectionType, ShouldRejectOnlyComputedFieldsWhenComputedFieldsAreBanned) {
    ASSERT_THROWS(makeProjectionWithBannedComputedFields(
                      BSON("a" << wrapInLiteral(1) << "b" << wrapInLiteral(2))),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithBannedComputedFields(
                      BSON("a.b" << wrapInLiteral(1) << "a.c" << wrapInLiteral(2))),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithBannedComputedFields(
                      BSON("a" << BSON("b" << wrapInLiteral(1) << "c" << wrapInLiteral(2)))),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithBannedComputedFields(
                      BSON("a" << BSON("b" << wrapInLiteral(1) << "c" << wrapInLiteral(2)))),
                  AssertionException);
}

TEST(ParsedAggregationProjectionType, ShouldAcceptInclusionProjectionWhenComputedFieldsAreBanned) {
    auto parsedProject = makeProjectionWithBannedComputedFields(BSON("a" << true));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithBannedComputedFields(BSON("_id" << false << "a" << true));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithBannedComputedFields(BSON("_id" << false << "a.b.c" << true));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithBannedComputedFields(BSON("_id.x" << true));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithBannedComputedFields(BSON("_id" << BSON("x" << true)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithBannedComputedFields(BSON("x" << BSON("_id" << true)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);
}

TEST(ParsedAggregationProjectionType, ShouldAcceptExclusionProjectionWhenComputedFieldsAreBanned) {
    auto parsedProject = makeProjectionWithBannedComputedFields(BSON("a" << false));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject = makeProjectionWithBannedComputedFields(BSON("_id.x" << false));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject = makeProjectionWithBannedComputedFields(BSON("_id" << BSON("x" << false)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject = makeProjectionWithBannedComputedFields(BSON("x" << BSON("_id" << false)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject = makeProjectionWithBannedComputedFields(BSON("_id" << false));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);
}

TEST(ParsedAggregationProjectionType, ShouldCoerceNumericsToBools) {
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

TEST(ParsedAggregationProjectionType, GetExpressionForPathGetsTopLevelExpression) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto projectObj = BSON("$add" << BSON_ARRAY(BSON("$const" << 1) << BSON("$const" << 3)));
    auto expr = Expression::parseObject(expCtx, projectObj, expCtx->variablesParseState);
    ParsedAggregationProjection::ProjectionPolicies defaultPolicies;
    auto node = InclusionNode(defaultPolicies);
    node.addExpressionForPath(FieldPath("key"), expr);
    BSONObjBuilder bob;
    ASSERT_EQ(expr, node.getExpressionForPath(FieldPath("key")));
}

TEST(ParsedAggregationProjectionType, GetExpressionForPathGetsCorrectTopLevelExpression) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto correctObj = BSON("$add" << BSON_ARRAY(BSON("$const" << 1) << BSON("$const" << 3)));
    auto incorrectObj = BSON("$add" << BSON_ARRAY(BSON("$const" << 2) << BSON("$const" << 4)));
    auto correctExpr = Expression::parseObject(expCtx, correctObj, expCtx->variablesParseState);
    auto incorrectExpr = Expression::parseObject(expCtx, incorrectObj, expCtx->variablesParseState);
    ParsedAggregationProjection::ProjectionPolicies defaultPolicies;
    auto node = InclusionNode(defaultPolicies);
    node.addExpressionForPath(FieldPath("key"), correctExpr);
    node.addExpressionForPath(FieldPath("other"), incorrectExpr);
    BSONObjBuilder bob;
    ASSERT_EQ(correctExpr, node.getExpressionForPath(FieldPath("key")));
}

TEST(ParsedAggregationProjectionType, GetExpressionForPathGetsNonTopLevelExpression) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto projectObj = BSON("$add" << BSON_ARRAY(BSON("$const" << 1) << BSON("$const" << 3)));
    auto expr = Expression::parseObject(expCtx, projectObj, expCtx->variablesParseState);
    ParsedAggregationProjection::ProjectionPolicies defaultPolicies;
    auto node = InclusionNode(defaultPolicies);
    node.addExpressionForPath(FieldPath("key.second"), expr);
    BSONObjBuilder bob;
    ASSERT_EQ(expr, node.getExpressionForPath(FieldPath("key.second")));
}

}  // namespace
}  // namespace parsed_aggregation_projection
}  // namespace mongo
