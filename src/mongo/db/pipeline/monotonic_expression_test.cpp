// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/monotonic_expression.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <memory>


namespace mongo {
namespace {

class MonotonicExpressionFixture : public ServiceContextMongoDTest {
public:
    bool isMonotonicExpression(BSONObj expressionSpec, const FieldPath& monotonicField) {
        auto expression =
            Expression::parseExpression(&_expCtx, expressionSpec, _expCtx.variablesParseState);
        return expression->isMonotonic(monotonicField);
    }

private:
    ExpressionContextForTest _expCtx;
};

}  // namespace

TEST_F(MonotonicExpressionFixture, ConstIsMonotonic) {
    ASSERT_TRUE(isMonotonicExpression(BSON("$const" << 1), "a"));
}

TEST_F(MonotonicExpressionFixture, MonotonicFieldIsMonotonic) {
    ASSERT_TRUE(isMonotonicExpression(BSON("$add" << BSON_ARRAY("$a")), "a"));
}

TEST_F(MonotonicExpressionFixture, NonMonotonicFieldIsNonMonotonic) {
    ASSERT_FALSE(isMonotonicExpression(BSON("$add" << BSON_ARRAY("$b")), "a"));
}

TEST_F(MonotonicExpressionFixture, MonotonicWithOppositeSignIsStillMonotonic) {
    ASSERT_TRUE(isMonotonicExpression(BSON("$subtract" << BSON_ARRAY(1 << "$a")), "a"));
}

TEST_F(MonotonicExpressionFixture, NestedOppositesAreProcessedCorrectlyForMonotonic) {
    // Test expression is: dateDiff(1 - (1 - (1 - a))), a)
    const auto& startDate = fromjson("{$subtract: [1, {$subtract: [1, {$subtract: [1, '$a']}]}]}");
    const auto& expression = BSON("$dateDiff" << BSON("startDate" << startDate << "endDate"
                                                                  << "$a"
                                                                  << "unit"
                                                                  << "hours"));
    ASSERT_TRUE(isMonotonicExpression(expression, "a"));
}

TEST_F(MonotonicExpressionFixture, NestedOppositesAreProcessedCorrectlyForNonMonotonic) {
    // LHS expression (1 - (1 - floor(a)) - 1 is increasing assuming $a is increasing.
    const auto& lhs =
        fromjson("{$subtract: [{$subtract: [1, {$subtract: [1, {$floor: '$a'}]}]}, 1]}");
    // RHS expression 1 - (1 - (1 - ceil(a))) is decreasing assuming $a is decreasing.
    const auto& rhs =
        fromjson("{$subtract: [1, {$subtract: [1, {$subtract: [1, {$ceil: '$a'}]}]}]}");
    // Both LHS and RHS are monotonic
    ASSERT_TRUE(isMonotonicExpression(lhs, "a"));
    ASSERT_TRUE(isMonotonicExpression(rhs, "a"));
    // Because they are monotonic in different directions, their sum is non monotonic, but their
    // difference is monotonic.
    ASSERT_FALSE(isMonotonicExpression(BSON("$add" << BSON_ARRAY(lhs << rhs)), "a"));
    ASSERT_TRUE(isMonotonicExpression(BSON("$subtract" << BSON_ARRAY(lhs << rhs)), "a"));
}

TEST_F(MonotonicExpressionFixture, MonotonicAndNonMonotonicFieldIsNonMonotonic) {
    ASSERT_FALSE(isMonotonicExpression(BSON("$add" << BSON_ARRAY("$a" << "$b")), "a"));
}

TEST_F(MonotonicExpressionFixture, MonotonicAndConstantIsMonotonic) {
    ASSERT_TRUE(isMonotonicExpression(BSON("$add" << BSON_ARRAY("$a" << "1")), "a"));
}

TEST_F(MonotonicExpressionFixture, ConstantAndConstantIsMonotonic) {
    ASSERT_TRUE(isMonotonicExpression(BSON("$add" << BSON_ARRAY("1" << "2")), "a"));
}

TEST_F(MonotonicExpressionFixture, MonotonicAndOppositeMonotonicIsNonMonotonic) {
    ASSERT_FALSE(isMonotonicExpression(BSON("$subtract" << BSON_ARRAY("$a" << "$a")), "a"));
}

TEST_F(MonotonicExpressionFixture, MonotonicAndMonotonicIsMonotonic) {
    const auto& dateTrunc = BSON("$dateTrunc" << BSON("date" << "$time"
                                                             << "unit"
                                                             << "hour"
                                                             << "timezone"
                                                             << "America/New_York"));
    ASSERT_TRUE(isMonotonicExpression(BSON("$add" << BSON_ARRAY(dateTrunc << "$time")), "time"));
    ASSERT_TRUE(isMonotonicExpression(BSON("$add" << BSON_ARRAY("$time" << "$time")), "time"));
}
TEST_F(MonotonicExpressionFixture, FunctionWithConstantNonMonotonicChildrenIsMonotonic) {
    ASSERT_TRUE(isMonotonicExpression(BSON("$dateTrunc" << BSON("date" << "$time"
                                                                       << "unit"
                                                                       << "hour"
                                                                       << "timezone"
                                                                       << "America/New_York")),
                                      "time"));
}

TEST_F(MonotonicExpressionFixture, FunctionWithNonConstantNonMonotonicChildrenIsNonMonotonic) {
    ASSERT_FALSE(isMonotonicExpression(BSON("$dateTrunc" << BSON("date" << "$time"
                                                                        << "unit"
                                                                        << "hour"
                                                                        << "binSize"
                                                                        << "$time")),
                                       "time"));
}

}  // namespace mongo
