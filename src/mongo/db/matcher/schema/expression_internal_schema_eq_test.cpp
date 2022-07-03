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

#include "mongo/bson/json.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(InternalSchemaEqMatchExpression, CorrectlyMatchesScalarElements) {
    BSONObj numberOperand = BSON("a" << 5);

    InternalSchemaEqMatchExpression eqNumberOperand("a", numberOperand["a"]);
    ASSERT_TRUE(eqNumberOperand.matchesBSON(BSON("a" << 5.0)));
    ASSERT_FALSE(eqNumberOperand.matchesBSON(BSON("a" << 6)));

    BSONObj stringOperand = BSON("a"
                                 << "str");

    InternalSchemaEqMatchExpression eqStringOperand("a", stringOperand["a"]);
    ASSERT_TRUE(eqStringOperand.matchesBSON(BSON("a"
                                                 << "str")));
    ASSERT_FALSE(eqStringOperand.matchesBSON(BSON("a"
                                                  << "string")));
}

TEST(InternalSchemaEqMatchExpression, CorrectlyMatchesArrayElement) {
    BSONObj operand = BSON("a" << BSON_ARRAY("b" << 5));

    InternalSchemaEqMatchExpression eq("a", operand["a"]);
    ASSERT_TRUE(eq.matchesBSON(BSON("a" << BSON_ARRAY("b" << 5))));
    ASSERT_FALSE(eq.matchesBSON(BSON("a" << BSON_ARRAY(5 << "b"))));
    ASSERT_FALSE(eq.matchesBSON(BSON("a" << BSON_ARRAY("b" << 5 << 5))));
    ASSERT_FALSE(eq.matchesBSON(BSON("a" << BSON_ARRAY("b" << 6))));
}

TEST(InternalSchemaEqMatchExpression, CorrectlyMatchesNullElement) {
    BSONObj operand = BSON("a" << BSONNULL);

    InternalSchemaEqMatchExpression eq("a", operand["a"]);
    ASSERT_TRUE(eq.matchesBSON(BSON("a" << BSONNULL)));
    ASSERT_FALSE(eq.matchesBSON(BSON("a" << 4)));
}

TEST(InternalSchemaEqMatchExpression, NullElementDoesNotMatchMissing) {
    BSONObj operand = BSON("a" << BSONNULL);

    InternalSchemaEqMatchExpression eq("a", operand["a"]);
    ASSERT_FALSE(eq.matchesBSON(BSONObj()));
    ASSERT_FALSE(eq.matchesBSON(BSON("b" << 4)));
}

TEST(InternalSchemaEqMatchExpression, NullElementDoesNotMatchUndefinedOrMissing) {
    BSONObj operand = BSON("a" << BSONNULL);

    InternalSchemaEqMatchExpression eq("a", operand["a"]);
    ASSERT_FALSE(eq.matchesBSON(BSONObj()));
    ASSERT_FALSE(eq.matchesBSON(fromjson("{a: undefined}")));
}

TEST(InternalSchemaEqMatchExpression, DoesNotTraverseLeafArrays) {
    BSONObj operand = BSON("a" << 5);
    InternalSchemaEqMatchExpression eq("a", operand["a"]);
    ASSERT_TRUE(eq.matchesBSON(BSON("a" << 5.0)));
    ASSERT_FALSE(eq.matchesBSON(BSON("a" << BSON_ARRAY(5))));
}

TEST(InternalSchemaEqMatchExpression, MatchesObjectsIndependentOfFieldOrder) {
    BSONObj operand = fromjson("{a: {b: 1, c: {d: 2, e: 3}}}");

    InternalSchemaEqMatchExpression eq("a", operand["a"]);
    ASSERT_TRUE(eq.matchesBSON(fromjson("{a: {b: 1, c: {d: 2, e: 3}}}")));
    ASSERT_TRUE(eq.matchesBSON(fromjson("{a: {c: {e: 3, d: 2}, b: 1}}")));
    ASSERT_FALSE(eq.matchesBSON(fromjson("{a: {b: 1, c: {d: 2}, e: 3}}")));
    ASSERT_FALSE(eq.matchesBSON(fromjson("{a: {b: 2, c: {d: 2}}}")));
    ASSERT_FALSE(eq.matchesBSON(fromjson("{a: {b: 1}}")));
}

TEST(InternalSchemaEqMatchExpression, EquivalentReturnsCorrectResults) {
    auto query = fromjson(R"(
             {a: {$_internalSchemaEq: {
                 b: {c: 1, d: 1}
             }}})");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher eqExpr(query, expCtx);

    query = fromjson(R"(
             {a: {$_internalSchemaEq: {
                 b: {d: 1, c: 1}
             }}})");
    Matcher eqExprEq(query, expCtx);
    ASSERT_TRUE(eqExpr.getMatchExpression()->equivalent(eqExprEq.getMatchExpression()));

    query = fromjson(R"(
             {a: {$_internalSchemaEq: {
                 b: {d: 1}
             }}})");
    Matcher eqExprNotEq(query, expCtx);
    ASSERT_FALSE(eqExpr.getMatchExpression()->equivalent(eqExprNotEq.getMatchExpression()));
}

TEST(InternalSchemaEqMatchExpression, EquivalentToClone) {
    auto query = fromjson("{a: {$_internalSchemaEq: {a:1, b: {c: 1, d: [1]}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher rootDocEq(query, expCtx);
    auto clone = rootDocEq.getMatchExpression()->shallowClone();
    ASSERT_TRUE(rootDocEq.getMatchExpression()->equivalent(clone.get()));
}

DEATH_TEST_REGEX(InternalSchemaEqMatchExpression,
                 GetChildFailsLargerThanZero,
                 "Tripwire assertion.*6400213") {
    BSONObj operand = BSON("a" << 5);
    InternalSchemaEqMatchExpression eq("a", operand["a"]);

    ASSERT_EQ(eq.numChildren(), 0);
    ASSERT_THROWS_CODE(eq.getChild(0), AssertionException, 6400213);
}

}  // namespace
}  // namespace mongo
