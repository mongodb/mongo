// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/expression_test_feature_flags.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string_view>


namespace mongo {

using ExpressionTestFeatureFlagsTest = AggregationContextFixture;
namespace {
const std::vector<std::string_view> expressionNames = {ExpressionTestFeatureFlagLastLTS::kName,
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
