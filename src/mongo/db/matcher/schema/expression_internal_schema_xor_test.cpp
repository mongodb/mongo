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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {
namespace {


TEST(InternalSchemaXorOp, MatchesNothingWhenHasNoClauses) {
    InternalSchemaXorMatchExpression internalSchemaXorOp;
    ASSERT_FALSE(exec::matcher::matchesBSON(&internalSchemaXorOp, BSONObj()));
}

TEST(InternalSchemaXorOp, MatchesSingleClause) {
    BSONObj matchPredicate = fromjson("{$_internalSchemaXor: [{a: { $ne: 5 }}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(matchPredicate, expCtx);

    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << 4)));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << BSON_ARRAY(4 << 6))));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << 5)));
    ASSERT_FALSE(
        exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << BSON_ARRAY(4 << 5))));
}

TEST(InternalSchemaXorOp, MatchesThreeClauses) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj matchPredicate =
        fromjson("{$_internalSchemaXor: [{a: { $gt: 10 }}, {a: { $lt: 0 }}, {b: 0}]}");

    auto expr = MatchExpressionParser::parse(matchPredicate, expCtx);

    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << -1)));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << 11)));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << 5)));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("b" << 100)));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("b" << 101)));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), BSONObj()));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << 11 << "b" << 100)));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << 11 << "b" << 0)));
}

TEST(InternalSchemaXorOp, MatchesSingleElement) {
    BSONObj matchPredicate = fromjson("{$_internalSchemaXor: [{a: {$lt: 5 }}, {b: 10}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(matchPredicate, expCtx);

    ASSERT_OK(expr.getStatus());
    BSONObj match1 = BSON("a" << 4);
    BSONObj match2 = BSON("b" << 10);
    BSONObj noMatch1 = BSON("a" << 8);
    BSONObj noMatch2 = BSON("c"
                            << "x");

    ASSERT_TRUE(expr.getValue()->matchesSingleElement(match1.firstElement()));
    ASSERT_TRUE(expr.getValue()->matchesSingleElement(match2.firstElement()));
    ASSERT_FALSE(expr.getValue()->matchesSingleElement(noMatch1.firstElement()));
    ASSERT_FALSE(expr.getValue()->matchesSingleElement(noMatch2.firstElement()));
}

TEST(InternalSchemaXorOp, DoesNotUseElemMatchKey) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    BSONObj matchPredicate = fromjson("{$_internalSchemaXor: [{a: 1}, {b: 2}]}");

    auto expr = MatchExpressionParser::parse(matchPredicate, expCtx);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << 1), &details));
    ASSERT_FALSE(details.hasElemMatchKey());
    ASSERT_TRUE(exec::matcher::matchesBSON(
        expr.getValue().get(), BSON("a" << BSON_ARRAY(1) << "b" << BSON_ARRAY(10)), &details));
    ASSERT_FALSE(details.hasElemMatchKey());
    ASSERT_FALSE(exec::matcher::matchesBSON(
        expr.getValue().get(), BSON("a" << BSON_ARRAY(3) << "b" << BSON_ARRAY(4)), &details));
    ASSERT_FALSE(details.hasElemMatchKey());
}

TEST(InternalSchemaXorOp, Equivalent) {
    BSONObj baseOperand1 = BSON("a" << 1);
    BSONObj baseOperand2 = BSON("b" << 2);
    EqualityMatchExpression sub1("a"_sd, baseOperand1["a"]);
    EqualityMatchExpression sub2("b"_sd, baseOperand2["b"]);

    InternalSchemaXorMatchExpression e1;
    e1.add(sub1.clone());
    e1.add(sub2.clone());

    InternalSchemaXorMatchExpression e2;
    e2.add(sub1.clone());

    ASSERT(e1.equivalent(&e1));
    ASSERT_FALSE(e1.equivalent(&e2));
}
}  // namespace
}  // namespace mongo
