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
void runTest(BSONObj spec, bool expectedResult) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj specObject = BSON("" << spec);
    BSONElement specElement = specObject.firstElement();
    VariablesParseState vps = expCtx.variablesParseState;
    boost::intrusive_ptr<Expression> expression =
        Expression::parseOperand(&expCtx, specElement, vps);
    ASSERT_BSONOBJ_EQ(BSON("" << expectedResult),
                      toBson(expression->evaluate(fromBson(BSON("a" << 1)), &expCtx.variables)));
    boost::intrusive_ptr<Expression> optimized = expression->optimize();
    ASSERT_BSONOBJ_EQ(BSON("" << expectedResult),
                      toBson(optimized->evaluate(fromBson(BSON("a" << 1)), &expCtx.variables)));
}
}  // namespace

using EvaluateAndTest = AggregationContextFixture;

TEST_F(EvaluateAndTest, NoOperands) {
    /** $and without operands. */
    runTest(BSON("$and" << BSONArray()), true);
}

TEST_F(EvaluateAndTest, True) {
    /** $and passed 'true'. */
    runTest(BSON("$and" << BSON_ARRAY(true)), true);
}

TEST_F(EvaluateAndTest, False) {
    /** $and passed 'false'. */
    runTest(BSON("$and" << BSON_ARRAY(false)), false);
}

TEST_F(EvaluateAndTest, TrueTrue) {
    /** $and passed 'true', 'true'. */
    runTest(BSON("$and" << BSON_ARRAY(true << true)), true);
}

TEST_F(EvaluateAndTest, TrueFalse) {
    /** $and passed 'true', 'false'. */
    runTest(BSON("$and" << BSON_ARRAY(true << false)), false);
}

TEST_F(EvaluateAndTest, FalseTrue) {
    /** $and passed 'false', 'true'. */
    runTest(BSON("$and" << BSON_ARRAY(false << true)), false);
}

TEST_F(EvaluateAndTest, FalseFalse) {
    /** $and passed 'false', 'false'. */
    runTest(BSON("$and" << BSON_ARRAY(false << false)), false);
}

TEST_F(EvaluateAndTest, TrueTrueTrue) {
    /** $and passed 'true', 'true', 'true'. */
    runTest(BSON("$and" << BSON_ARRAY(true << true << true)), true);
}

TEST_F(EvaluateAndTest, TrueTrueFalse) {
    /** $and passed 'true', 'true', 'false'. */
    runTest(BSON("$and" << BSON_ARRAY(true << true << false)), false);
}

TEST_F(EvaluateAndTest, ZeroOne) {
    /** $and passed '0', '1'. */
    runTest(BSON("$and" << BSON_ARRAY(0 << 1)), false);
}

TEST_F(EvaluateAndTest, OneTwo) {
    /** $and passed '1', '2'. */
    runTest(BSON("$and" << BSON_ARRAY(1 << 2)), true);
}

TEST_F(EvaluateAndTest, FieldPath) {
    /** $and passed a field path. */
    runTest(BSON("$and" << BSON_ARRAY("$a")), true);
}

}  // namespace expression_evaluation_test
}  // namespace mongo
