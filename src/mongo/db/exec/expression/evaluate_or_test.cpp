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
using boost::intrusive_ptr;

void runTest(BSONObj spec, bool expectedResult) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj specObject = BSON("" << spec);
    BSONElement specElement = specObject.firstElement();
    VariablesParseState vps = expCtx.variablesParseState;
    intrusive_ptr<Expression> expression = Expression::parseOperand(&expCtx, specElement, vps);
    ASSERT_BSONOBJ_EQ(BSON("" << expectedResult),
                      toBson(expression->evaluate(fromBson(BSON("a" << 1)), &expCtx.variables)));
    intrusive_ptr<Expression> optimized = expression->optimize();
    ASSERT_BSONOBJ_EQ(BSON("" << expectedResult),
                      toBson(optimized->evaluate(fromBson(BSON("a" << 1)), &expCtx.variables)));
}
}  // namespace

using EvaluateOrTest = AggregationContextFixture;

TEST_F(EvaluateOrTest, NoOperands) {
    /** $or without operands. */
    runTest(BSON("$or" << BSONArray()), false);
}

TEST_F(EvaluateOrTest, True) {
    /** $or passed 'true'. */
    runTest(BSON("$or" << BSON_ARRAY(true)), true);
}

TEST_F(EvaluateOrTest, False) {
    /** $or passed 'false'. */
    runTest(BSON("$or" << BSON_ARRAY(false)), false);
}

TEST_F(EvaluateOrTest, TrueTrue) {
    /** $or passed 'true', 'true'. */
    runTest(BSON("$or" << BSON_ARRAY(true << true)), true);
}

TEST_F(EvaluateOrTest, TrueFalse) {
    /** $or passed 'true', 'false'. */
    runTest(BSON("$or" << BSON_ARRAY(true << false)), true);
}

TEST_F(EvaluateOrTest, FalseTrue) {
    /** $or passed 'false', 'true'. */
    runTest(BSON("$or" << BSON_ARRAY(false << true)), true);
}

TEST_F(EvaluateOrTest, FalseFalse) {
    /** $or passed 'false', 'false'. */
    runTest(BSON("$or" << BSON_ARRAY(false << false)), false);
}

TEST_F(EvaluateOrTest, FalseFalseFalse) {
    /** $or passed 'false', 'false', 'false'. */
    runTest(BSON("$or" << BSON_ARRAY(false << false << false)), false);
}

TEST_F(EvaluateOrTest, FalseFalseTrue) {
    /** $or passed 'false', 'false', 'true'. */
    runTest(BSON("$or" << BSON_ARRAY(false << false << true)), true);
}

TEST_F(EvaluateOrTest, ZeroOne) {
    /** $or passed '0', '1'. */
    runTest(BSON("$or" << BSON_ARRAY(0 << 1)), true);
}

TEST_F(EvaluateOrTest, ZeroFalse) {
    /** $or passed '0', 'false'. */
    runTest(BSON("$or" << BSON_ARRAY(0 << false)), false);
}

TEST_F(EvaluateOrTest, FieldPath) {
    /** $or passed a field path. */
    runTest(BSON("$or" << BSON_ARRAY("$a")), true);
}

}  // namespace expression_evaluation_test
}  // namespace mongo
