/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/parsed_match_expression_for_test.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_shape.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
void assertShapeIs(std::string filterJson, BSONObj expectedShape) {
    ParsedMatchExpressionForTest expr(filterJson);
    ASSERT_BSONOBJ_EQ(expectedShape, query_shape::predicateShape(expr.get()));
}

void assertShapeIs(std::string filterJson, std::string expectedShapeJson) {
    return assertShapeIs(filterJson, fromjson(expectedShapeJson));
}

}  // namespace

TEST(QueryPredicateShape, Equals) {
    assertShapeIs("{a: 5}", "{a: {$eq: '?'}}");                                   // Implicit equals
    assertShapeIs("{a: {$eq: 5}}", "{a: {$eq: '?'}}");                            // Explicit equals
    assertShapeIs("{a: 5, b: 6}", "{$and: [{a: {$eq: '?'}}, {b: {$eq: '?'}}]}");  // implicit $and
}

TEST(QueryPredicateShape, Comparisons) {
    assertShapeIs("{a: {$lt: 5}, b: {$gt: 6}, c: {$gte: 3, $lte: 10}}",
                  "{$and: [{a: {$lt: '?'}}, {b: {$gt: '?'}}, {c: {$gte: '?'}}, {c: {$lte: '?'}}]}");
}

TEST(QueryPredicateShape, Regex) {
    // Note/warning: 'fromjson' will parse $regex into a /regex/. If you want to keep it as-is,
    // construct the BSON yourself.
    assertShapeIs("{a: /a+/}", BSON("a" << BSON("$regex" << query_shape::kLiteralArgString)));
    assertShapeIs("{a: /a+/i}",
                  BSON("a" << BSON("$regex" << query_shape::kLiteralArgString << "$options"
                                            << query_shape::kLiteralArgString)));
}

TEST(QueryPredicateShape, Mod) {
    assertShapeIs("{a: {$mod: [2, 0]}}", "{a: {$mod: '?'}}");
}

TEST(QueryPredicateShape, Exists) {
    assertShapeIs("{a: {$exists: true}}", "{a: {$exists: '?'}}");
}

TEST(QueryPredicateShape, In) {
    // Any number of children is always the same shape
    assertShapeIs("{a: {$in: [1]}}", "{a: {$in: ['?']}}");
    assertShapeIs("{a: {$in: [1, 4, 'str', /regex/]}}", "{a: {$in: ['?']}}");
}

TEST(QueryPredicateShape, BitTestOperators) {
    assertShapeIs("{a: {$bitsAllSet: [1, 5]}}", "{a: {$bitsAllSet: '?'}}");
    assertShapeIs("{a: {$bitsAllSet: 50}}", "{a: {$bitsAllSet: '?'}}");

    assertShapeIs("{a: {$bitsAnySet: [1, 5]}}", "{a: {$bitsAnySet: '?'}}");
    assertShapeIs("{a: {$bitsAnySet: 50}}", "{a: {$bitsAnySet: '?'}}");

    assertShapeIs("{a: {$bitsAllClear: [1, 5]}}", "{a: {$bitsAllClear: '?'}}");
    assertShapeIs("{a: {$bitsAllClear: 50}}", "{a: {$bitsAllClear: '?'}}");

    assertShapeIs("{a: {$bitsAnyClear: [1, 5]}}", "{a: {$bitsAnyClear: '?'}}");
    assertShapeIs("{a: {$bitsAnyClear: 50}}", "{a: {$bitsAnyClear: '?'}}");
}

BSONObj queryShapeForOptimizedExprExpression(std::string exprPredicateJson) {
    ParsedMatchExpressionForTest expr(exprPredicateJson);
    // We need to optimize an $expr expression in order to generate an $_internalExprEq. It's not
    // clear we'd want to do optimization before computing the query shape, but we should support
    // the computation on any MatchExpression, and this is the easiest way we can create this type
    // of MatchExpression node.
    auto optimized = MatchExpression::optimize(expr.release());
    return query_shape::predicateShape(optimized.get());
}

TEST(QueryPredicateShape, OptimizedExprPredicates) {
    // TODO SERVER-73709 $expr should respect the literal redaction and hide the '2' - here and in
    // all following assertions.
    ASSERT_BSONOBJ_EQ(
        queryShapeForOptimizedExprExpression("{$expr: {$eq: ['$a', 2]}}"),
        fromjson("{$and: [{a: {$_internalExprEq: '?'}}, {$expr: {$eq: ['$a', {$const: 2}]}}]}"));

    ASSERT_BSONOBJ_EQ(
        queryShapeForOptimizedExprExpression("{$expr: {$lt: ['$a', 2]}}"),
        fromjson("{$and: [{a: {$_internalExprLt: '?'}}, {$expr: {$lt: ['$a', {$const: 2}]}}]}"));

    ASSERT_BSONOBJ_EQ(
        queryShapeForOptimizedExprExpression("{$expr: {$lte: ['$a', 2]}}"),
        fromjson("{$and: [{a: {$_internalExprLte: '?'}}, {$expr: {$lte: ['$a', {$const: 2}]}}]}"));

    ASSERT_BSONOBJ_EQ(
        queryShapeForOptimizedExprExpression("{$expr: {$gt: ['$a', 2]}}"),
        fromjson("{$and: [{a: {$_internalExprGt: '?'}}, {$expr: {$gt: ['$a', {$const: 2}]}}]}"));

    ASSERT_BSONOBJ_EQ(
        queryShapeForOptimizedExprExpression("{$expr: {$gte: ['$a', 2]}}"),
        fromjson("{$and: [{a: {$_internalExprGte: '?'}}, {$expr: {$gte: ['$a', {$const: 2}]}}]}"));
}

}  // namespace mongo
