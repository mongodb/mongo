/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/bson/json.h"
#include "mongo/db/matcher/rewrite_expr.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

/**
 * Attempts to rewrite an Expression ('expr') and confirms that the resulting
 * MatchExpression ('expectedMatch') is as expected. As it is not always possible to completely
 * rewrite an Expression to MatchExpression the result can represent one of:
 *   - A full rewrite
 *   - A partial rewrite matching on a superset of documents
 *   - No MatchExpression (when full or superset rewrite is not possible)
 */
void testExprRewrite(BSONObj expr, BSONObj expectedMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    auto expression =
        Expression::parseOperand(expCtx, expr.firstElement(), expCtx->variablesParseState);

    auto result = RewriteExpr::rewrite(expression, expCtx->getCollator());

    // Confirm expected match.
    if (!expectedMatch.isEmpty()) {
        ASSERT(result.matchExpression());
        BSONObjBuilder bob;
        result.matchExpression()->serialize(&bob);
        ASSERT_BSONOBJ_EQ(expectedMatch, bob.obj());
    } else {
        ASSERT_FALSE(result.matchExpression());
    }
}

//
// Expressions that can be fully translated to MatchExpression.
//
TEST(RewriteExpr, EqWithOneFieldPathRewritesToMatch) {
    BSONObj expr = fromjson("{$expr: {$eq: ['$x', 3]}}");
    const BSONObj expectedMatch = fromjson("{x: {$eq: 3}}");
    testExprRewrite(expr, expectedMatch);

    expr = fromjson("{$expr: {$eq: [3, '$x']}}");
    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, NeWithOneFieldPathRewritesToMatch) {
    BSONObj expr = fromjson("{$expr: {$ne: ['$x', 3]}}");
    BSONObj expectedMatch = fromjson("{$nor: [{x: {$eq: 3}}]}");
    testExprRewrite(expr, expectedMatch);

    expr = fromjson("{$expr: {$ne: [3, '$x']}}");
    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, GtWithOneFieldPathRewritesToMatch) {
    BSONObj expr = fromjson("{$expr: {$gt: ['$x', 3]}}");
    BSONObj expectedMatch = fromjson("{x: {$gt: 3}}");
    testExprRewrite(expr, expectedMatch);

    expr = fromjson("{$expr: {$gt: [3, '$x']}}");
    expectedMatch = fromjson("{x: {$lt: 3}}");
    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, GteWithOneFieldPathRewritesToMatch) {
    BSONObj expr = fromjson("{$expr: {$gte: ['$x', 3]}}");
    BSONObj expectedMatch = fromjson("{x: {$gte: 3}}");
    testExprRewrite(expr, expectedMatch);

    expr = fromjson("{$expr: {$gte: [3, '$x']}}");
    expectedMatch = fromjson("{x: {$lte: 3}}");
    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, LtWithOneFieldPathRewritesToMatch) {
    BSONObj expr = fromjson("{$expr: {$lt: ['$x', 3]}}");
    BSONObj expectedMatch = fromjson("{x: {$lt: 3}}");
    testExprRewrite(expr, expectedMatch);

    expr = fromjson("{$expr: {$lt: [3, '$x']}}");
    expectedMatch = fromjson("{x: {$gt: 3}}");
    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, LteWithOneFieldPathRewritesToMatch) {
    BSONObj expr = fromjson("{$expr: {$lte: ['$x', 3]}}");
    BSONObj expectedMatch = fromjson("{x: {$lte: 3}}");
    testExprRewrite(expr, expectedMatch);

    expr = fromjson("{$expr: {$lte: [3, '$x']}}");
    expectedMatch = fromjson("{x: {$gte: 3}}");
    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, AndRewritesToMatch) {
    const BSONObj expr = fromjson("{$expr: {$and: [{$eq: ['$x', 3]}, {$ne: ['$y', 4]}]}}");
    const BSONObj expectedMatch = fromjson("{$and: [{x: {$eq: 3}}, {$nor: [{y: {$eq: 4}}]}]}");

    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, OrRewritesToMatch) {
    const BSONObj expr = fromjson("{$expr: {$or: [{$lte: ['$x', 3]}, {$gte: ['$y', 4]}]}}");
    const BSONObj expectedMatch = fromjson("{$or: [{x: {$lte: 3}}, {y: {$gte: 4}}]}");

    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, AndNestedWithinOrRewritesToMatch) {
    const BSONObj expr = fromjson(
        "{$expr: {$or: [{$and: [{$eq: ['$x', 3]}, {$gt: ['$z', 5]}]}, {$lt: ['$y', 4]}]}}");
    const BSONObj expectedMatch =
        fromjson("{$or: [{$and: [{x: {$eq: 3}}, {z: {$gt: 5}}]}, {y: {$lt: 4}}]}");

    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, OrNestedWithinAndRewritesToMatch) {
    const BSONObj expr = fromjson(
        "{$expr: {$and: [{$or: [{$eq: ['$x', 3]}, {$eq: ['$z', 5]}]}, {$eq: ['$y', 4]}]}}");
    const BSONObj expectedMatch =
        fromjson("{$and: [{$or: [{x: {$eq: 3}}, {z: {$eq: 5}}]}, {y: {$eq: 4}}]}");

    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, InWithLhsFieldPathRewritesToMatch) {
    const BSONObj expr = fromjson("{$expr: {$in: ['$x', [1, 2, 3]]}}");
    const BSONObj expectedMatch = fromjson("{x: {$in: [1, 2, 3]}}");

    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, InWithLhsFieldPathAndArrayAsConstRewritesToMatch) {
    const BSONObj expr = fromjson("{$expr: {$in: ['$x', {$const: [1, 2, 3]}]}}");
    const BSONObj expectedMatch = fromjson("{x: {$in: [1, 2, 3]}}");

    testExprRewrite(expr, expectedMatch);
}


//
// Expressions that cannot be rewritten (partially or fully) to MatchExpression.
//

TEST(RewriteExpr, CmpDoesNotRewriteToMatch) {
    const BSONObj expr = fromjson("{$expr: {$cmp: ['$x', 3]}}");
    const BSONObj expectedMatch;

    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, ConstantExpressionDoesNotRewriteToMatch) {
    const BSONObj expr = fromjson("{$expr: 1}");
    const BSONObj expectedMatch;

    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, EqWithTwoFieldPathsDoesNotRewriteToMatch) {
    const BSONObj expr = fromjson("{$expr: {$eq: ['$x', '$y']}}");
    const BSONObj expectedMatch;

    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, EqWithTwoConstantsDoesNotRewriteToMatch) {
    const BSONObj expr = fromjson("{$expr: {$eq: [3, 4]}}");
    const BSONObj expectedMatch;

    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, EqWithDottedFieldPathDoesNotRewriteToMatch) {
    const BSONObj expr = fromjson("{$expr: {$eq: ['$x.y', 3]}}");
    const BSONObj expectedMatch;

    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, InWithDottedFieldPathDoesNotRewriteToMatch) {
    const BSONObj expr = fromjson("{$expr: {$in: ['$x.y', [1, 2, 3]]}}");
    const BSONObj expectedMatch;

    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, AndWithoutMatchSubtreeDoesNotRewriteToMatch) {
    const BSONObj expr = fromjson("{$expr: {$and: [{$eq: ['$w', '$x']}, {$eq: ['$y', '$z']}]}}");
    const BSONObj expectedMatch;

    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, OrWithDistinctMatchAndNonMatchSubTreeDoesNotRewriteToMatch) {
    const BSONObj expr = fromjson("{$expr: {$or: [{$eq: ['$x', 1]}, {$eq: ['$y', '$z']}]}}");
    const BSONObj expectedMatch;

    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, InWithoutLhsFieldPathDoesNotRewriteToMatch) {
    const BSONObj expr = fromjson("{$expr: {$in: [2, [1, 2, 3]]}}");
    const BSONObj expectedMatch;

    testExprRewrite(expr, expectedMatch);
}

//
// Expressions that can be partially rewritten to MatchExpression. Partial rewrites are expected to
// match against a superset of documents.
//

TEST(RewriteExpr, NestedAndWithTwoFieldPathsWithinOrPartiallyRewriteToMatch) {
    const BSONObj expr = fromjson(
        "{$expr: {$or: [{$and: [{$eq: ['$x', '$w']}, {$eq: ['$z', 5]}]}, {$eq: ['$y', 4]}]}}");
    const BSONObj expectedMatch = fromjson("{$or: [{z: {$eq: 5}}, {y: {$eq: 4}}]}");

    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, AndWithDistinctMatchAndNonMatchSubTreeSplitsOnRewrite) {
    const BSONObj expr = fromjson("{$expr: {$and: [{$eq: ['$x', 1]}, {$eq: ['$y', '$z']}]}}");
    const BSONObj expectedMatch = fromjson("{x: {$eq: 1}}");

    testExprRewrite(expr, expectedMatch);
}

// Rewrites an Expression that contains a mix of rewritable and non-rewritable statements to a
// MatchExpression superset.
TEST(RewriteExpr, ComplexSupersetMatchRewritesToMatchSuperset) {
    const BSONObj expr = fromjson(
        "{"
        "  $expr: {"
        "      $and: ["
        "          {$eq: ['$a', 1]},"
        "          {$eq: ['$b', '$c']},"
        "          {"
        "            $or: ["
        "                {$eq: ['$d', 1]},"
        "                {$eq: ['$e', 3]},"
        "                {"
        "                  $and: ["
        "                      {$eq: ['$f', 1]},"
        "                      {$eq: ['$g', '$h']},"
        "                      {$or: [{$eq: ['$i', 3]}, {$eq: ['$j', '$k']}]}"
        "                  ]"
        "                }"
        "            ]"
        "          }"
        "      ]"
        "  }"
        "}");
    const BSONObj expectedMatch = fromjson(
        "{"
        "  $and: ["
        "      {a: {$eq: 1}},"
        "      {"
        "        $or: ["
        "            {d: {$eq: 1}},"
        "            {e: {$eq: 3}},"
        "            {f: {$eq: 1}}"
        "        ]"
        "      }"
        "  ]"
        "}");

    testExprRewrite(expr, expectedMatch);
}

TEST(RewriteExpr, OrWithAndContainingMatchAndNonMatchChildPartiallyRewritesToMatch) {
    const BSONObj expr = fromjson(
        "{$expr: {$or: [{$eq: ['$x', 3]}, {$and: [{$eq: ['$y', 4]}, {$eq: ['$y', '$z']}]}]}}");
    const BSONObj expectedMatch = fromjson("{$or: [{x: {$eq: 3}}, {y: {$eq: 4}}]}");

    testExprRewrite(expr, expectedMatch);
}

}  // namespace
}  // namespace mongo
