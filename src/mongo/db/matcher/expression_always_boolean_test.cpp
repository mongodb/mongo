/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/platform/basic.h"

#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(AlwaysFalseMatchExpression, RejectsAllObjects) {
    AlwaysFalseMatchExpression falseExpr;

    ASSERT_FALSE(falseExpr.matchesBSON(BSON("a" << BSONObj())));
    ASSERT_FALSE(falseExpr.matchesBSON(BSON("a" << 1)));
    ASSERT_FALSE(falseExpr.matchesBSON(BSON("a"
                                            << "string")));
    ASSERT_FALSE(falseExpr.matchesBSON(BSONObj()));
}

TEST(AlwaysFalseMatchExpression, EquivalentReturnsCorrectResults) {
    auto falseExpr = std::make_unique<AlwaysFalseMatchExpression>();
    ASSERT_TRUE(falseExpr->equivalent(falseExpr.get()));
    ASSERT_TRUE(falseExpr->equivalent(falseExpr->shallowClone().get()));

    AlwaysTrueMatchExpression trueExpr;
    ASSERT_FALSE(falseExpr->equivalent(&trueExpr));
}

TEST(AlwaysTrueMatchExpression, AcceptsAllObjects) {
    AlwaysTrueMatchExpression trueExpr;

    ASSERT_TRUE(trueExpr.matchesBSON(BSON("a" << BSONObj())));
    ASSERT_TRUE(trueExpr.matchesBSON(BSON("a" << 1)));
    ASSERT_TRUE(trueExpr.matchesBSON(BSON("a"
                                          << "string")));
    ASSERT_TRUE(trueExpr.matchesBSON(BSONObj()));
}

TEST(AlwaysTrueMatchExpression, EquivalentReturnsCorrectResults) {
    auto trueExpr = std::make_unique<AlwaysTrueMatchExpression>();
    ASSERT_TRUE(trueExpr->equivalent(trueExpr.get()));
    ASSERT_TRUE(trueExpr->equivalent(trueExpr->shallowClone().get()));

    AlwaysFalseMatchExpression falseExpr;
    ASSERT_FALSE(trueExpr->equivalent(&falseExpr));
}

DEATH_TEST_REGEX(AlwaysTrueMatchExpression,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400202") {
    auto trueExpr = std::make_unique<AlwaysTrueMatchExpression>();

    ASSERT_EQ(trueExpr->numChildren(), 0);
    ASSERT_THROWS_CODE(trueExpr->getChild(0), AssertionException, 6400202);
}

DEATH_TEST_REGEX(AlwaysFalseMatchExpression,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400202") {
    auto falseExpr = std::make_unique<AlwaysFalseMatchExpression>();

    ASSERT_EQ(falseExpr->numChildren(), 0);
    ASSERT_THROWS_CODE(falseExpr->getChild(0), AssertionException, 6400202);
}

}  // namespace
}  // namespace mongo
