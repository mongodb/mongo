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

#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

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
    auto clone = rootDocEq.getMatchExpression()->clone();
    ASSERT_TRUE(rootDocEq.getMatchExpression()->equivalent(clone.get()));
}

DEATH_TEST_REGEX(InternalSchemaEqMatchExpression,
                 GetChildFailsLargerThanZero,
                 "Tripwire assertion.*6400213") {
    BSONObj operand = BSON("a" << 5);
    InternalSchemaEqMatchExpression eq("a"_sd, operand["a"]);

    ASSERT_EQ(eq.numChildren(), 0);
    ASSERT_THROWS_CODE(eq.getChild(0), AssertionException, 6400213);
}

}  // namespace
}  // namespace mongo
