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

#include "mongo/db/pipeline/expression_test_feature_flags.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using ExpressionTestFeatureFlagsTest = AggregationContextFixture;
namespace {
const std::vector<StringData> expressionNames = {ExpressionTestFeatureFlagLastLTS::kName,
                                                 ExpressionTestFeatureFlagLatest::kName};
}

TEST_F(ExpressionTestFeatureFlagsTest, LatestFCVParseAssertConstraints) {
    for (auto& exprName : expressionNames) {
        {
            // Non-numeric inputs are not allowed.
            auto expCtx = getExpCtx();
            auto emptyObj = BSON(exprName << BSONObj());
            ASSERT_THROWS_CODE(
                Expression::parseExpression(expCtx.get(), emptyObj, expCtx->variablesParseState),
                AssertionException,
                10445700);
        }
        {
            // The numeric input must equal 1.
            auto expCtx = getExpCtx();
            auto negativeNumObj = BSON(exprName << -0.12);
            ASSERT_THROWS_CODE(Expression::parseExpression(
                                   expCtx.get(), negativeNumObj, expCtx->variablesParseState),
                               AssertionException,
                               10445700);
        }
    }
}

TEST_F(ExpressionTestFeatureFlagsTest, EvaluatesToOneSimpleExpression) {
    for (auto& exprName : expressionNames) {
        auto expCtx = getExpCtx();
        auto spec = BSON(exprName << 1);
        auto expr = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        auto result = expr->evaluate(Document{}, &expCtx->variables);
        ASSERT_VALUE_EQ(result, Value(1));
    }
}

TEST_F(ExpressionTestFeatureFlagsTest, EvaluatesToOneNestedExpression) {
    for (auto& exprName : expressionNames) {
        auto expCtx = getExpCtx();
        auto spec = BSON("$gt" << BSON_ARRAY(BSON(exprName << 1) << BSON("$ceil" << 7.8)));
        auto expr = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        auto result = expr->evaluate(Document{}, &expCtx->variables);
        ASSERT_VALUE_EQ(result, Value(false));
    }
}

}  // namespace mongo
