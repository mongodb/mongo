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
