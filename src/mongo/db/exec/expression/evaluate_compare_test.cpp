// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/expression/evaluate_test_helpers.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace expression_evaluation_test {

namespace {
void runTest(BSONObj spec, BSONObj expectedResult) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj specObject = BSON("" << spec);
    BSONElement specElement = specObject.firstElement();
    VariablesParseState vps = expCtx.variablesParseState;
    boost::intrusive_ptr<Expression> expression =
        Expression::parseOperand(&expCtx, specElement, vps);
    // Check evaluation result.
    ASSERT_BSONOBJ_EQ(expectedResult, toBson(expression->evaluate({}, &expCtx.variables)));
    // Check that the result is the same after optimizing.
    boost::intrusive_ptr<Expression> optimized = expression->optimize();
    ASSERT_BSONOBJ_EQ(expectedResult, toBson(optimized->evaluate({}, &expCtx.variables)));
}
}  // namespace

using EvaluateCompareTest = AggregationContextFixture;

TEST_F(EvaluateCompareTest, EqLt) {
    /** $eq with first < second. */
    runTest(BSON("$eq" << BSON_ARRAY(1 << 2)), BSON("" << false));
}

TEST_F(EvaluateCompareTest, EqEq) {
    /** $eq with first == second. */
    runTest(BSON("$eq" << BSON_ARRAY(1 << 1)), BSON("" << true));
}

TEST_F(EvaluateCompareTest, EqGt) {
    /** $eq with first > second. */
    runTest(BSON("$eq" << BSON_ARRAY(1 << 0)), BSON("" << false));
}

TEST_F(EvaluateCompareTest, NeLt) {
    /** $ne with first < second. */
    runTest(BSON("$ne" << BSON_ARRAY(1 << 2)), BSON("" << true));
}

TEST_F(EvaluateCompareTest, NeEq) {
    /** $ne with first == second. */
    runTest(BSON("$ne" << BSON_ARRAY(1 << 1)), BSON("" << false));
}

TEST_F(EvaluateCompareTest, NeGt) {
    /** $ne with first > second. */
    runTest(BSON("$ne" << BSON_ARRAY(1 << 0)), BSON("" << true));
}

TEST_F(EvaluateCompareTest, GtLt) {
    /** $gt with first < second. */
    runTest(BSON("$gt" << BSON_ARRAY(1 << 2)), BSON("" << false));
}

TEST_F(EvaluateCompareTest, GtEq) {
    /** $gt with first == second. */
    runTest(BSON("$gt" << BSON_ARRAY(1 << 1)), BSON("" << false));
}

TEST_F(EvaluateCompareTest, GtGt) {
    /** $gt with first > second. */
    runTest(BSON("$gt" << BSON_ARRAY(1 << 0)), BSON("" << true));
}

TEST_F(EvaluateCompareTest, GteLt) {
    /** $gte with first < second. */
    runTest(BSON("$gte" << BSON_ARRAY(1 << 2)), BSON("" << false));
}

TEST_F(EvaluateCompareTest, GteEq) {
    /** $gte with first == second. */
    runTest(BSON("$gte" << BSON_ARRAY(1 << 1)), BSON("" << true));
}

TEST_F(EvaluateCompareTest, GteGt) {
    /** $gte with first > second. */
    runTest(BSON("$gte" << BSON_ARRAY(1 << 0)), BSON("" << true));
}

TEST_F(EvaluateCompareTest, LtLt) {
    /** $lt with first < second. */
    runTest(BSON("$lt" << BSON_ARRAY(1 << 2)), BSON("" << true));
}

TEST_F(EvaluateCompareTest, LtEq) {
    /** $lt with first == second. */
    runTest(BSON("$lt" << BSON_ARRAY(1 << 1)), BSON("" << false));
}

TEST_F(EvaluateCompareTest, LtGt) {
    /** $lt with first > second. */
    runTest(BSON("$lt" << BSON_ARRAY(1 << 0)), BSON("" << false));
}

TEST_F(EvaluateCompareTest, LteLt) {
    /** $lte with first < second. */
    runTest(BSON("$lte" << BSON_ARRAY(1 << 2)), BSON("" << true));
}

TEST_F(EvaluateCompareTest, LteEq) {
    /** $lte with first == second. */
    runTest(BSON("$lte" << BSON_ARRAY(1 << 1)), BSON("" << true));
}

TEST_F(EvaluateCompareTest, LteGt) {
    /** $lte with first > second. */
    runTest(BSON("$lte" << BSON_ARRAY(1 << 0)), BSON("" << false));
}

TEST_F(EvaluateCompareTest, CmpLt) {
    /** $cmp with first < second. */
    runTest(BSON("$cmp" << BSON_ARRAY(1 << 2)), BSON("" << -1));
}

TEST_F(EvaluateCompareTest, CmpEq) {
    /** $cmp with first == second. */
    runTest(BSON("$cmp" << BSON_ARRAY(1 << 1)), BSON("" << 0));
}

TEST_F(EvaluateCompareTest, CmpGt) {
    /** $cmp with first > second. */
    runTest(BSON("$cmp" << BSON_ARRAY(1 << 0)), BSON("" << 1));
}

TEST_F(EvaluateCompareTest, CmpBracketed) {
    /** $cmp results are bracketed to an absolute value of 1. */
    runTest(BSON("$cmp" << BSON_ARRAY("z" << "a")), BSON("" << 1));
}

}  // namespace expression_evaluation_test
}  // namespace mongo
