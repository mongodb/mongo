/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
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
