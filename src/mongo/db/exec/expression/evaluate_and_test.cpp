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
