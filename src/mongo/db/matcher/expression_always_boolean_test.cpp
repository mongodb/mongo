// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/matcher/expression_always_boolean.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo {

namespace {

TEST(AlwaysFalseMatchExpression, EquivalentReturnsCorrectResults) {
    auto falseExpr = std::make_unique<AlwaysFalseMatchExpression>();
    ASSERT_TRUE(falseExpr->equivalent(falseExpr.get()));
    ASSERT_TRUE(falseExpr->equivalent(falseExpr->clone().get()));

    AlwaysTrueMatchExpression trueExpr;
    ASSERT_FALSE(falseExpr->equivalent(&trueExpr));
}

TEST(AlwaysTrueMatchExpression, EquivalentReturnsCorrectResults) {
    auto trueExpr = std::make_unique<AlwaysTrueMatchExpression>();
    ASSERT_TRUE(trueExpr->equivalent(trueExpr.get()));
    ASSERT_TRUE(trueExpr->equivalent(trueExpr->clone().get()));

    AlwaysFalseMatchExpression falseExpr;
    ASSERT_FALSE(trueExpr->equivalent(&falseExpr));
}

DEATH_TEST_REGEX(AlwaysTrueMatchExpressionDeathTest,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400202") {
    auto trueExpr = std::make_unique<AlwaysTrueMatchExpression>();

    ASSERT_EQ(trueExpr->numChildren(), 0);
    ASSERT_THROWS_CODE(trueExpr->getChild(0), AssertionException, 6400202);
}

DEATH_TEST_REGEX(AlwaysFalseMatchExpressionDeathTest,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400202") {
    auto falseExpr = std::make_unique<AlwaysFalseMatchExpression>();

    ASSERT_EQ(falseExpr->numChildren(), 0);
    ASSERT_THROWS_CODE(falseExpr->getChild(0), AssertionException, 6400202);
}

}  // namespace
}  // namespace mongo
