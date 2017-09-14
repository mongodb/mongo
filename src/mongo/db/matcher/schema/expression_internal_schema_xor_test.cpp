/**
 *    Copyright (C) 2017 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::unique_ptr;

TEST(InternalSchemaXorOp, MatchesNothingWhenHasNoClauses) {
    InternalSchemaXorMatchExpression internalSchemaXorOp;
    ASSERT_FALSE(internalSchemaXorOp.matchesBSON(BSONObj()));
}

TEST(InternalSchemaXorOp, MatchesSingleClause) {
    BSONObj matchPredicate = fromjson("{$_internalSchemaXor: [{a: { $ne: 5 }}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(matchPredicate, expCtx);

    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(expr.getValue()->matchesBSON(BSON("a" << 4)));
    ASSERT_TRUE(expr.getValue()->matchesBSON(BSON("a" << BSON_ARRAY(4 << 6))));
    ASSERT_FALSE(expr.getValue()->matchesBSON(BSON("a" << 5)));
    ASSERT_FALSE(expr.getValue()->matchesBSON(BSON("a" << BSON_ARRAY(4 << 5))));
}

TEST(InternalSchemaXorOp, MatchesThreeClauses) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj matchPredicate =
        fromjson("{$_internalSchemaXor: [{a: { $gt: 10 }}, {a: { $lt: 0 }}, {b: 0}]}");

    auto expr = MatchExpressionParser::parse(matchPredicate, expCtx);

    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(expr.getValue()->matchesBSON(BSON("a" << -1)));
    ASSERT_TRUE(expr.getValue()->matchesBSON(BSON("a" << 11)));
    ASSERT_FALSE(expr.getValue()->matchesBSON(BSON("a" << 5)));
    ASSERT_FALSE(expr.getValue()->matchesBSON(BSON("b" << 100)));
    ASSERT_FALSE(expr.getValue()->matchesBSON(BSON("b" << 101)));
    ASSERT_FALSE(expr.getValue()->matchesBSON(BSONObj()));
    ASSERT_TRUE(expr.getValue()->matchesBSON(BSON("a" << 11 << "b" << 100)));
    ASSERT_FALSE(expr.getValue()->matchesBSON(BSON("a" << 11 << "b" << 0)));
}

TEST(InternalSchemaXorOp, DoesNotUseElemMatchKey) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    BSONObj matchPredicate = fromjson("{$_internalSchemaXor: [{a: 1}, {b: 2}]}");

    auto expr = MatchExpressionParser::parse(matchPredicate, expCtx);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(expr.getValue()->matchesBSON(BSON("a" << 1), &details));
    ASSERT_FALSE(details.hasElemMatchKey());
    ASSERT_TRUE(expr.getValue()->matchesBSON(BSON("a" << BSON_ARRAY(1) << "b" << BSON_ARRAY(10)),
                                             &details));
    ASSERT_FALSE(details.hasElemMatchKey());
    ASSERT_FALSE(
        expr.getValue()->matchesBSON(BSON("a" << BSON_ARRAY(3) << "b" << BSON_ARRAY(4)), &details));
    ASSERT_FALSE(details.hasElemMatchKey());
}

TEST(InternalSchemaXorOp, Equivalent) {
    BSONObj baseOperand1 = BSON("a" << 1);
    BSONObj baseOperand2 = BSON("b" << 2);
    EqualityMatchExpression sub1;
    ASSERT(sub1.init("a", baseOperand1["a"]).isOK());
    EqualityMatchExpression sub2;
    ASSERT(sub2.init("b", baseOperand2["b"]).isOK());

    InternalSchemaXorMatchExpression e1;
    e1.add(sub1.shallowClone().release());
    e1.add(sub2.shallowClone().release());

    InternalSchemaXorMatchExpression e2;
    e2.add(sub1.shallowClone().release());

    ASSERT(e1.equivalent(&e1));
    ASSERT_FALSE(e1.equivalent(&e2));
}
}  // namespace
}  // namespace mongo
