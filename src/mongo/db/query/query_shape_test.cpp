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
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/matcher/parsed_match_expression_for_test.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

/**
 * Simplistic redaction strategy for testing which appends the field name to the prefix "REDACT_".
 */
std::string redactFieldNameForTest(StringData sd) {
    return "REDACT_" + sd.toString();
}

static const SerializationOptions literalAndFieldRedactOpts{redactFieldNameForTest,
                                                            query_shape::kLiteralArgString};


BSONObj predicateShape(std::string filterJson) {
    ParsedMatchExpressionForTest expr(filterJson);
    return query_shape::predicateShape(expr.get());
}

BSONObj predicateShapeRedacted(std::string filterJson) {
    ParsedMatchExpressionForTest expr(filterJson);
    return query_shape::predicateShape(expr.get(), redactFieldNameForTest);
}

#define ASSERT_SHAPE_EQ_AUTO(expected, actual) \
    ASSERT_BSONOBJ_EQ_AUTO(expected, predicateShape(actual))

#define ASSERT_REDACTED_SHAPE_EQ_AUTO(expected, actual) \
    ASSERT_BSONOBJ_EQ_AUTO(expected, predicateShapeRedacted(actual))

}  // namespace

TEST(QueryPredicateShape, Equals) {
    ASSERT_SHAPE_EQ_AUTO(  // Implicit equals
        R"({"a":{"$eq":"?"}})",
        "{a: 5}");
    ASSERT_SHAPE_EQ_AUTO(  // Explicit equals
        R"({"a":{"$eq":"?"}})",
        "{a: {$eq: 5}}");
    ASSERT_SHAPE_EQ_AUTO(  // implicit $and
        R"({"$and":[{"a":{"$eq":"?"}},{"b":{"$eq":"?"}}]})",
        "{a: 5, b: 6}");
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // Implicit equals
        R"({"REDACT_a":{"$eq":"?"}})",
        "{a: 5}");
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // Explicit equals
        R"({"REDACT_a":{"$eq":"?"}})",
        "{a: {$eq: 5}}");
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // NOLINT
        R"({"$and":[{"REDACT_a":{"$eq":"?"}},{"REDACT_b":{"$eq":"?"}}]})",
        "{a: 5, b: 6}");
}

TEST(QueryPredicateShape, Comparisons) {
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({
            "$and": [
                {
                    "a": {
                        "$lt": "?"
                    }
                },
                {
                    "b": {
                        "$gt": "?"
                    }
                },
                {
                    "c": {
                        "$gte": "?"
                    }
                },
                {
                    "c": {
                        "$lte": "?"
                    }
                }
            ]
        })",
        "{a: {$lt: 5}, b: {$gt: 6}, c: {$gte: 3, $lte: 10}}");
}

namespace {
void assertShapeIs(std::string filterJson, BSONObj expectedShape) {
    ParsedMatchExpressionForTest expr(filterJson);
    ASSERT_BSONOBJ_EQ(expectedShape, query_shape::predicateShape(expr.get()));
}

void assertRedactedShapeIs(std::string filterJson, BSONObj expectedShape) {
    ParsedMatchExpressionForTest expr(filterJson);
    ASSERT_BSONOBJ_EQ(expectedShape,
                      query_shape::predicateShape(expr.get(), redactFieldNameForTest));
}
}  // namespace

TEST(QueryPredicateShape, Regex) {
    // Note/warning: 'fromjson' will parse $regex into a /regex/, so these tests can't use
    // auto-updating BSON assertions.
    assertShapeIs("{a: /a+/}", BSON("a" << BSON("$regex" << query_shape::kLiteralArgString)));
    assertShapeIs("{a: /a+/i}",
                  BSON("a" << BSON("$regex" << query_shape::kLiteralArgString << "$options"
                                            << query_shape::kLiteralArgString)));
    assertRedactedShapeIs("{a: /a+/}",
                          BSON("REDACT_a" << BSON("$regex" << query_shape::kLiteralArgString)));
}

TEST(QueryPredicateShape, Mod) {
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$mod":"?"}})",
        "{a: {$mod: [2, 0]}}");
}

TEST(QueryPredicateShape, Exists) {
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$exists":"?"}})",
        "{a: {$exists: true}}");
}

TEST(QueryPredicateShape, In) {
    // Any number of children is always the same shape
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$in":["?"]}})",
        "{a: {$in: [1]}}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$in":["?"]}})",
        "{a: {$in: [1, 4, 'str', /regex/]}}");
}

TEST(QueryPredicateShape, BitTestOperators) {
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$bitsAllSet":"?"}})",
        "{a: {$bitsAllSet: [1, 5]}}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$bitsAllSet":"?"}})",
        "{a: {$bitsAllSet: 50}}");

    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$bitsAnySet":"?"}})",
        "{a: {$bitsAnySet: [1, 5]}}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$bitsAnySet":"?"}})",
        "{a: {$bitsAnySet: 50}}");

    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$bitsAllClear":"?"}})",
        "{a: {$bitsAllClear: [1, 5]}}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$bitsAllClear":"?"}})",
        "{a: {$bitsAllClear: 50}}");

    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$bitsAnyClear":"?"}})",
        "{a: {$bitsAnyClear: [1, 5]}}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$bitsAnyClear":"?"}})",
        "{a: {$bitsAnyClear: 50}}");
}

TEST(QueryPredicateShape, AlwaysBoolean) {
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"$alwaysTrue":"?"})",
        "{$alwaysTrue: 1}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"$alwaysFalse":"?"})",
        "{$alwaysFalse: 1}");
}

TEST(QueryPredicateShape, And) {
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"$and":[{"a":{"$lt":"?"}},{"b":{"$gte":"?"}},{"c":{"$lte":"?"}}]})",
        "{$and: [{a: {$lt: 5}}, {b: {$gte: 3}}, {c: {$lte: 10}}]}");
}

TEST(QueryPredicateShape, Or) {
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"$or":[{"a":{"$eq":"?"}},{"b":{"$in":["?"]}},{"c":{"$gt":"?"}}]})",
        "{$or: [{a: 5}, {b: {$in: [1,2,3]}}, {c: {$gt: 10}}]}");
}

TEST(QueryPredicateShape, ElemMatch) {
    // ElemMatchObjectMatchExpression
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$elemMatch":{"$and":[{"b":{"$eq":"?"}},{"c":{"$exists":"?"}}]}}})",
        "{a: {$elemMatch: {b: 5, c: {$exists: true}}}}");

    // ElemMatchValueMatchExpression
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$elemMatch":{"$gt":"?","$lt":"?"}}})",
        "{a: {$elemMatch: {$gt: 5, $lt: 10}}}");

    // Nested
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // NOLINT
        R"({"REDACT_a":{"$elemMatch":{"$elemMatch":{"$gt":"?","$lt":"?"}}}})",
        "{a: {$elemMatch: {$elemMatch: {$gt: 5, $lt: 10}}}}");
}

TEST(QueryPredicateShape, InternalBucketGeoWithinMatchExpression) {
    auto query =
        "{ $_internalBucketGeoWithin: {withinRegion: {$centerSphere: [[0, 0], 10]}, field: \"a\"} "
        "}";
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // NOLINT
        R"({
            "$_internalBucketGeoWithin": {
                "withinRegion": {
                    "$centerSphere": "?"
                },
                "field": "REDACT_a"
            }
        })",
        query);
}

TEST(QueryPredicateShape, NorMatchExpression) {
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // NOLINT
        R"({"$nor":[{"REDACT_a":{"$lt":"?"}},{"REDACT_b":{"$gt":"?"}}]})",
        "{ $nor: [ { a: {$lt: 5} }, { b: {$gt: 4} } ] }");
}

TEST(QueryPredicateShape, NotMatchExpression) {
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // NOLINT
        R"({"REDACT_price":{"$not":{"$gt":"?"}}})",
        "{ price: { $not: { $gt: 1.99 } } }");
    // Test the special case where NotMatchExpression::serialize() reduces to $alwaysFalse.
    auto emptyAnd = std::make_unique<AndMatchExpression>();
    const MatchExpression& notExpr = NotMatchExpression(std::move(emptyAnd));
    auto serialized = notExpr.serialize(literalAndFieldRedactOpts);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$alwaysFalse":"?"})",
        serialized);
}

TEST(QueryPredicateShape, SizeMatchExpression) {
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // NOLINT
        R"({"REDACT_price":{"$size":"?"}})",
        "{ price: { $size: 2 } }");
}

TEST(QueryPredicateShape, TextMatchExpression) {
    TextMatchExpressionBase::TextParams params = {"coffee"};
    auto expr = ExtensionsCallbackNoop().createText(params);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$text": {
                "$search": "?",
                "$language": "?",
                "$caseSensitive": "?",
                "$diacriticSensitive": "?"
            }
        })",
        expr->serialize(literalAndFieldRedactOpts));
}

TEST(QueryPredicateShape, TwoDPtInAnnulusExpression) {
    const MatchExpression& expr = TwoDPtInAnnulusExpression({}, {});
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$TwoDPtInAnnulusExpression":true})",
        expr.serialize(literalAndFieldRedactOpts));
}

TEST(QueryPredicateShape, WhereMatchExpression) {
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"$where":"?"})",
        "{$where: \"some_code()\"}");
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
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$and": [
                {
                    "a": {
                        "$_internalExprEq": "?"
                    }
                },
                {
                    "$expr": {
                        "$eq": [
                            "$a",
                            {
                                "$const": "?"
                            }
                        ]
                    }
                }
            ]
        })",
        queryShapeForOptimizedExprExpression("{$expr: {$eq: ['$a', 2]}}"));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$and": [
                {
                    "a": {
                        "$_internalExprLt": "?"
                    }
                },
                {
                    "$expr": {
                        "$lt": [
                            "$a",
                            {
                                "$const": "?"
                            }
                        ]
                    }
                }
            ]
        })",
        queryShapeForOptimizedExprExpression("{$expr: {$lt: ['$a', 2]}}"));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$and": [
                {
                    "a": {
                        "$_internalExprLte": "?"
                    }
                },
                {
                    "$expr": {
                        "$lte": [
                            "$a",
                            {
                                "$const": "?"
                            }
                        ]
                    }
                }
            ]
        })",
        queryShapeForOptimizedExprExpression("{$expr: {$lte: ['$a', 2]}}"));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$and": [
                {
                    "a": {
                        "$_internalExprGt": "?"
                    }
                },
                {
                    "$expr": {
                        "$gt": [
                            "$a",
                            {
                                "$const": "?"
                            }
                        ]
                    }
                }
            ]
        })",
        queryShapeForOptimizedExprExpression("{$expr: {$gt: ['$a', 2]}}"));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$and": [
                {
                    "a": {
                        "$_internalExprGte": "?"
                    }
                },
                {
                    "$expr": {
                        "$gte": [
                            "$a",
                            {
                                "$const": "?"
                            }
                        ]
                    }
                }
            ]
        })",
        queryShapeForOptimizedExprExpression("{$expr: {$gte: ['$a', 2]}}"));
}

}  // namespace mongo
