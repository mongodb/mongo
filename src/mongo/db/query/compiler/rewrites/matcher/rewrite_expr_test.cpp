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

#include "mongo/db/query/compiler/rewrites/matcher/rewrite_expr.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
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
void testExprRewrite(BSONObj expr, BSONObj expectedMatch, bool reversedInRewrite = false) {
    auto expCtx = ExpressionContextForTest{};
    expCtx.variablesParseState.defineVariable("var");
    expCtx.variablesParseState.defineVariable("abc");
    internalQueryExtraPredicateForReversedIn.store(reversedInRewrite);

    auto expression =
        Expression::parseOperand(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    // The $expr + $in rewrite expects the rhs array to a constant array (ExpressionConstant), so we
    // call optimize() here to ensure the Expression is in a valid format.
    expression = expression->optimize();

    auto result = RewriteExpr::rewrite(expression, expCtx.getCollator());

    // Confirm expected match.
    if (!expectedMatch.isEmpty()) {
        ASSERT(result.matchExpression());
        BSONObjBuilder bob;
        result.matchExpression()->serialize(&bob, {});
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
    const BSONObj expectedMatch = fromjson("{x: {$_internalExprEq: 3}}");
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);

    expr = fromjson("{$expr: {$eq: [3, '$x']}}");
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, AndRewritesToMatch) {
    const BSONObj expr = fromjson("{$expr: {$and: [{$eq: ['$x', 3]}, {$eq: ['$y', 4]}]}}");
    const BSONObj expectedMatch =
        fromjson("{$and: [{x: {$_internalExprEq: 3}}, {y: {$_internalExprEq: 4}}]}");

    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, OrRewritesToMatch) {
    const BSONObj expr = fromjson("{$expr: {$or: [{$eq: ['$x', 3]}, {$eq: ['$y', 4]}]}}");
    const BSONObj expectedMatch =
        fromjson("{$or: [{x: {$_internalExprEq: 3}}, {y: {$_internalExprEq: 4}}]}");

    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, AndNestedWithinOrRewritesToMatch) {
    const BSONObj expr = fromjson(
        "{$expr: {$or: [{$and: [{$eq: ['$x', 3]}, {$eq: ['$z', 5]}]}, {$eq: ['$y', 4]}]}}");
    const BSONObj expectedMatch = fromjson(
        "{$or: [{$and: [{x: {$_internalExprEq: 3}}, {z: {$_internalExprEq: 5}}]}, "
        "{y: {$_internalExprEq: 4}}]}");

    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, OrNestedWithinAndRewritesToMatch) {
    const BSONObj expr = fromjson(
        "{$expr: {$and: [{$or: [{$eq: ['$x', 3]}, {$eq: ['$z', 5]}]}, {$eq: ['$y', 4]}]}}");
    const BSONObj expectedMatch = fromjson(
        "{$and: [{$or: [{x: {$_internalExprEq: 3}}, {z: {$_internalExprEq: 5}}]}, "
        "{y: {$_internalExprEq: 4}}]}");

    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, EqWithDottedFieldPathRewritesToMatch) {
    const BSONObj expr = fromjson("{$expr: {$eq: ['$x.y', 3]}}");
    const BSONObj expectedMatch = fromjson("{'x.y': {$_internalExprEq: 3}}");

    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

//
// Expressions that cannot be rewritten (partially or fully) to MatchExpression.
//

TEST(RewriteExpr, CmpDoesNotRewriteToMatch) {
    const BSONObj expr = fromjson("{$expr: {$cmp: ['$x', 3]}}");
    const BSONObj expectedMatch;

    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, ConstantExpressionDoesNotRewriteToMatch) {
    const BSONObj expr = fromjson("{$expr: 1}");
    const BSONObj expectedMatch;

    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, EqWithTwoFieldPathsDoesNotRewriteToMatch) {
    const BSONObj expr = fromjson("{$expr: {$eq: ['$x', '$y']}}");
    const BSONObj expectedMatch;

    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, EqWithTwoConstantsDoesNotRewriteToMatch) {
    const BSONObj expr = fromjson("{$expr: {$eq: [3, 4]}}");
    const BSONObj expectedMatch;

    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, EqWithComparisonToUndefinedDoesNotRewriteToMatch) {
    BSONObj expr = fromjson("{$expr: {$eq: ['$x', undefined]}}");
    const BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, EqWithComparisonToMissingDoesNotRewriteToMatch) {
    BSONObj expr = fromjson("{$expr: {$eq: ['$x', '$$REMOVE']}}");
    const BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, EqWithComparisonToRootDoesNotRewriteToMatch) {
    BSONObj expr = fromjson("{$expr: {$eq: ['$$ROOT', {a: 1}]}}");
    const BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, EqWithComparisonToCurrentDoesNotRewriteToMatch) {
    BSONObj expr = fromjson("{$expr: {$eq: ['$$CURRENT', 2]}}");
    const BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, EqWithComparisonToArrayDoesNotRewriteToMatch) {
    BSONObj expr = fromjson("{$expr: {$eq: ['$x', [1, 2, 3]]}}");
    const BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, NeWithOneFieldPathDoesNotRewriteToMatch) {
    BSONObj expr = fromjson("{$expr: {$ne: ['$x', 3]}}");
    BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);

    expr = fromjson("{$expr: {$ne: [3, '$x']}}");
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, AndWithoutMatchSubtreeDoesNotRewriteToMatch) {
    const BSONObj expr = fromjson("{$expr: {$and: [{$eq: ['$w', '$x']}, {$eq: ['$y', '$z']}]}}");
    const BSONObj expectedMatch;

    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, OrWithDistinctMatchAndNonMatchSubTreeDoesNotRewriteToMatch) {
    const BSONObj expr = fromjson("{$expr: {$or: [{$eq: ['$x', 1]}, {$eq: ['$y', '$z']}]}}");
    const BSONObj expectedMatch;

    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

//
// Expressions that can be partially rewritten to MatchExpression. Partial rewrites are expected to
// match against a superset of documents.
//

TEST(RewriteExpr, NestedAndWithTwoFieldPathsWithinOrPartiallyRewriteToMatch) {
    const BSONObj expr = fromjson(
        "{$expr: {$or: [{$and: [{$eq: ['$x', '$w']}, {$eq: ['$z', 5]}]}, {$eq: ['$y', 4]}]}}");
    const BSONObj expectedMatch =
        fromjson("{$or: [{z: {$_internalExprEq: 5}}, {y: {$_internalExprEq: 4}}]}");

    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, AndWithDistinctMatchAndNonMatchSubTreeSplitsOnRewrite) {
    const BSONObj expr = fromjson("{$expr: {$and: [{$eq: ['$x', 1]}, {$eq: ['$y', '$z']}]}}");
    const BSONObj expectedMatch = fromjson("{x: {$_internalExprEq: 1}}");

    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
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
        "      {a: {$_internalExprEq: 1}},"
        "      {"
        "        $or: ["
        "            {d: {$_internalExprEq: 1}},"
        "            {e: {$_internalExprEq: 3}},"
        "            {f: {$_internalExprEq: 1}}"
        "        ]"
        "      }"
        "  ]"
        "}");

    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, OrWithAndContainingMatchAndNonMatchChildPartiallyRewritesToMatch) {
    const BSONObj expr = fromjson(
        "{$expr: {$or: [{$eq: ['$x', 3]}, {$and: [{$eq: ['$y', 4]}, {$eq: ['$y', '$z']}]}]}}");
    const BSONObj expectedMatch =
        fromjson("{$or: [{x: {$_internalExprEq: 3}}, {y: {$_internalExprEq: 4}}]}");

    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithScalarInListRewritesToMatch) {
    const BSONObj expr = fromjson("{$expr: {$in: ['$category', ['clothing', 'materials']]}}");
    const BSONObj expectedMatch = fromjson("{category: {$in: ['clothing', 'materials']}}");
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithDottedFieldPathRewritesToMatch) {
    const BSONObj expr = fromjson("{$expr: {$in: ['$category.a', ['clothing', 'materials']]}}");
    const BSONObj expectedMatch = fromjson("{'category.a': {$in: ['clothing', 'materials']}}");
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithObjectInListRewritesToMatch) {
    const BSONObj expr = fromjson("{$expr: {$in: ['$category', [{}, {a: 'clothing'}]]}}");
    const BSONObj expectedMatch = fromjson("{category: {$in: [{}, {a: 'clothing'}]}}");
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithArrayInListDoesNotRewriteToMatch) {
    // Arrays are unsupported because MatchExpressions have implicit array traversal semantics,
    // while agg expressions don't traverse arrays and will only match on direct equalities.
    const BSONObj expr = fromjson(
        "{$expr: {$in: ['$category', ['clothing', 'materials', ['clothing', 'electronics']]]}}");
    const BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithRegexInListDoesNotRewriteToMatch) {
    // Regexes are unsupported because MatchExpressions will evaluate the regex and match documents
    // that satisfy the regex (/clothing/ will match /clothing/, "clothing", and "clothings").
    // However, agg will only match on strict equality (/clothing/ only matches a value of
    // /clothing/).
    const BSONObj expr = fromjson("{$expr: {$in: ['$category', [/clothing/]]}}");
    const BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithNullInListDoesNotRewriteToMatch) {
    // Nulls are unsupported because MatchExpressions will also match on missing values, such as
    // documents without a "category" field. However, agg will only match documents whose value at
    // the field is null.
    const BSONObj expr = fromjson("{$expr: {$in: ['$category', [null]]}}");
    const BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithGetFieldFieldPathDoesNotRewriteToMatch) {
    // $getField can express field names with dots in them, while InMatchExpression can't, so we
    // can't rewrite this case.
    const BSONObj expr =
        fromjson("{$expr: {$in: [{$getField: 'category.a'}, ['clothing', 'electronics']]}}");
    const BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithFieldPathOnRHSGetsRewrittenToMatch) {
    const BSONObj expr = fromjson("{$expr: {$in: ['abc', '$foo']}}");
    // Note: it gets rewritten to {$in: ['abc']}, and then to {$eq: ['abc']}. This is correct
    // because of array traversal semantics in $match!
    BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    expectedMatch = fromjson("{'foo': {'$eq': 'abc'}}");
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithFieldPathOnRHSInOrGetsRewrittenToMatch) {
    const BSONObj expr = fromjson(
        "{$expr: {$or: [{$in: ['abc', '$foo']}, {$in: ['def', '$foo']}, {$in: ['ghi', '$foo']}]}}");
    BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    expectedMatch =
        fromjson("{'foo': {'$in': ['abc', 'def', 'ghi']}}");  // Note: multiple $ins coalesced by
                                                              // optimization.
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithFieldPathOnRHSInAndGetsRewrittenToMatch) {
    const BSONObj expr = fromjson(
        "{$expr: {$and: [{$in: ['abc', '$foo']}, {$in: ['def', '$foo']}, {$in: ['ghi', "
        "'$foo']}]}}");
    BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    expectedMatch = fromjson(
        "{$and: [{'foo': {'$eq': 'abc'}}, {'foo': {'$eq': 'def'}}, {'foo': {'$eq': "
        "'ghi'}}]}");
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithMultipleRHSFieldPathsInOrGetsRewrittenToMatch) {
    const BSONObj expr = fromjson(
        "{$expr: {$or: [{$in: ['abc', '$foo']}, {$in: ['def', '$foo']}, {$in: ['ghi', "
        "'$foo2']}]}}");
    BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    expectedMatch = fromjson("{$or: [{'foo': {'$in': ['abc', 'def']}}, {'foo2': {$eq: 'ghi'}}]}");
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithBothSidesAsFieldPathDoesNotGetRewrittenToMatch) {
    const BSONObj expr = fromjson("{$expr: {$in: ['$abc', '$foo']}}");
    const BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithBothSidesAsVariableDoesNotGetRewrittenToMatch) {
    const BSONObj expr = fromjson("{$expr: {$in: ['$$abc', '$$var']}}");
    const BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithBothSidesAsRootDoesNotGetRewrittenToMatch) {
    const BSONObj expr = fromjson("{$expr: {$in: ['$$ROOT', '$$ROOT']}}");
    const BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithRootAsRHSFieldPathDoesNotGetRewrittenToMatch) {
    const BSONObj expr = fromjson("{$expr: {$in: ['abc', '$$ROOT']}}");
    const BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithVariableAsRHSFieldPathDoesNotGetRewrittenToMatch) {
    const BSONObj expr = fromjson("{$expr: {$in: ['abc', '$$var']}}");
    const BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithVariableAsLHSFieldPathDoesNotGetRewrittenToMatch) {
    const BSONObj expr = fromjson("{$expr: {$in: ['$$var', ['abc']]}}");
    const BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithRootAsLHSFieldPathDoesNotGetRewrittenToMatch) {
    const BSONObj expr = fromjson("{$expr: {$in: ['$$ROOT', ['abc']]}}");
    const BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

TEST(RewriteExpr, InWithRegexAsLHSConstDoesNotGetRewrittenToMatch) {
    const BSONObj expr = fromjson("{$expr: {$in: [/myRegex/, '$foo']}}");
    const BSONObj expectedMatch;
    testExprRewrite(expr, expectedMatch);
    testExprRewrite(expr, expectedMatch, true);
}

}  // namespace
}  // namespace mongo
