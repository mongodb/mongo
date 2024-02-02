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
#include "mongo/db/query/query_shape_test_gen.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

/**
 * Simplistic redaction strategy for testing which appends the field name to the prefix "REDACT_".
 */
std::string applyHmacForTest(StringData sd) {
    return "REDACT_" + sd.toString();
}

static const SerializationOptions literalAndFieldRedactOpts{
    applyHmacForTest, LiteralSerializationPolicy::kToDebugTypeString};


BSONObj predicateShape(std::string filterJson) {
    ParsedMatchExpressionForTest expr(filterJson);
    return query_shape::debugPredicateShape(expr.get());
}

BSONObj predicateShapeRedacted(std::string filterJson) {
    ParsedMatchExpressionForTest expr(filterJson);
    return query_shape::debugPredicateShape(expr.get(), applyHmacForTest);
}

#define ASSERT_SHAPE_EQ_AUTO(expected, actual) \
    ASSERT_BSONOBJ_EQ_AUTO(expected, predicateShape(actual))

#define ASSERT_REDACTED_SHAPE_EQ_AUTO(expected, actual) \
    ASSERT_BSONOBJ_EQ_AUTO(expected, predicateShapeRedacted(actual))

}  // namespace

TEST(QueryPredicateShape, Equals) {
    ASSERT_SHAPE_EQ_AUTO(  // Implicit equals
        R"({"a":{"$eq":"?number"}})",
        "{a: 5}");
    ASSERT_SHAPE_EQ_AUTO(  // Explicit equals
        R"({"a":{"$eq":"?number"}})",
        "{a: {$eq: 5}}");
    ASSERT_SHAPE_EQ_AUTO(  // implicit $and
        R"({"$and":[{"a":{"$eq":"?number"}},{"b":{"$eq":"?number"}}]})",
        "{a: 5, b: 6}");
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // Implicit equals
        R"({"REDACT_a":{"$eq":"?number"}})",
        "{a: 5}");
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // Explicit equals
        R"({"REDACT_a":{"$eq":"?number"}})",
        "{a: {$eq: 5}}");
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // NOLINT
        R"({"$and":[{"REDACT_a":{"$eq":"?number"}},{"REDACT_b":{"$eq":"?number"}}]})",
        "{a: 5, b: 6}");
}

TEST(QueryPredicateShape, ArraySubTypes) {
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        "{a: {$eq: '[]'}}",
        "{a: []}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        "{a: {$eq: '?array<?number>'}}",
        "{a: [2]}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$eq":"?array<?number>"}})",
        "{a: [2, 3]}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$eq":"?array<?object>"}})",
        "{a: [{}]}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$eq":"?array<?object>"}})",
        "{a: [{}, {}]}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$eq":"?array<?array>"}})",
        "{a: [[], [], []]}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$eq":"?array<?array>"}})",
        "{a: [[2, 3], ['string'], []]}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$eq":"?array<>"}})",
        "{a: [{}, 2]}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$eq":"?array<>"}})",
        "{a: [[], 2]}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$eq":"?array<>"}})",
        "{a: [[{}, 'string'], 2]}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$eq":"?array<>"}})",
        "{a: [[{}, 'string'], 2]}");
}

TEST(QueryPredicateShape, Comparisons) {
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({
            "$and": [
                {
                    "a": {
                        "$lt": "?number"
                    }
                },
                {
                    "b": {
                        "$gt": "?number"
                    }
                },
                {
                    "c": {
                        "$gte": "?number"
                    }
                },
                {
                    "c": {
                        "$lte": "?number"
                    }
                }
            ]
        })",
        "{a: {$lt: 5}, b: {$gt: 6}, c: {$gte: 3, $lte: 10}}");
}

namespace {
void assertShapeIs(std::string filterJson, BSONObj expectedShape) {
    ParsedMatchExpressionForTest expr(filterJson);
    ASSERT_BSONOBJ_EQ(expectedShape, query_shape::debugPredicateShape(expr.get()));
}

void assertRedactedShapeIs(std::string filterJson, BSONObj expectedShape) {
    ParsedMatchExpressionForTest expr(filterJson);
    ASSERT_BSONOBJ_EQ(expectedShape,
                      query_shape::debugPredicateShape(expr.get(), applyHmacForTest));
}
}  // namespace

TEST(QueryPredicateShape, Regex) {
    // Note/warning: 'fromjson' will parse $regex into a /regex/, so these tests can't use
    // auto-updating BSON assertions.
    assertShapeIs("{a: /a+/}",
                  BSON("a" << BSON("$regex"
                                   << "?string")));
    assertShapeIs("{a: /a+/i}",
                  BSON("a" << BSON("$regex"
                                   << "?string"
                                   << "$options"
                                   << "?string")));
    assertRedactedShapeIs("{a: /a+/}",
                          BSON("REDACT_a" << BSON("$regex"
                                                  << "?string")));
    assertRedactedShapeIs("{a: /a+/}",
                          BSON("REDACT_a" << BSON("$regex"
                                                  << "?string")));
}

TEST(QueryPredicateShape, Mod) {
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$mod":["?number","?number"]}})",
        "{a: {$mod: [2, 0]}}");
}

TEST(QueryPredicateShape, Exists) {
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$exists":"?bool"}})",
        "{a: {$exists: true}}");
}

TEST(QueryPredicateShape, In) {
    // Any number of children is always the same shape
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$in":"?array<?number>"}})",
        "{a: {$in: [1]}}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$in":"?array<>"}})",
        "{a: {$in: [1, 4, 'str', /regex/]}}");
}

TEST(QueryPredicateShape, BitTestOperators) {
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$bitsAllSet":"?array<?number>"}})",
        "{a: {$bitsAllSet: [1, 5]}}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$bitsAllSet":"?array<?number>"}})",
        "{a: {$bitsAllSet: 50}}");

    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$bitsAnySet":"?array<?number>"}})",
        "{a: {$bitsAnySet: [1, 5]}}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$bitsAnySet":"?array<?number>"}})",
        "{a: {$bitsAnySet: 50}}");

    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$bitsAllClear":"?array<?number>"}})",
        "{a: {$bitsAllClear: [1, 5]}}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$bitsAllClear":"?array<?number>"}})",
        "{a: {$bitsAllClear: 50}}");

    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$bitsAnyClear":"?array<?number>"}})",
        "{a: {$bitsAnyClear: [1, 5]}}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$bitsAnyClear":"?array<?number>"}})",
        "{a: {$bitsAnyClear: 50}}");
}

TEST(QueryPredicateShape, AlwaysBoolean) {
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"$alwaysTrue":"?number"})",
        "{$alwaysTrue: 1}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"$alwaysFalse":"?number"})",
        "{$alwaysFalse: 1}");
}

TEST(QueryPredicateShape, And) {
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({
            "$and": [
                {
                    "a": {
                        "$lt": "?number"
                    }
                },
                {
                    "b": {
                        "$gte": "?number"
                    }
                },
                {
                    "c": {
                        "$lte": "?number"
                    }
                }
            ]
        })",
        "{$and: [{a: {$lt: 5}}, {b: {$gte: 3}}, {c: {$lte: 10}}]}");
}

TEST(QueryPredicateShape, Or) {
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({
            "$or": [
                {
                    "a": {
                        "$eq": "?number"
                    }
                },
                {
                    "b": {
                        "$in": "?array<?number>"
                    }
                },
                {
                    "c": {
                        "$gt": "?number"
                    }
                }
            ]
        })",
        "{$or: [{a: 5}, {b: {$in: [1,2,3]}}, {c: {$gt: 10}}]}");
}

TEST(QueryPredicateShape, ElemMatch) {
    // ElemMatchObjectMatchExpression
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({
            "a": {
                "$elemMatch": {
                    "$and": [
                        {
                            "b": {
                                "$eq": "?number"
                            }
                        },
                        {
                            "c": {
                                "$exists": "?bool"
                            }
                        }
                    ]
                }
            }
        })",
        "{a: {$elemMatch: {b: 5, c: {$exists: true}}}}");

    // ElemMatchValueMatchExpression
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$elemMatch":{"$gt":"?number","$lt":"?number"}}})",
        "{a: {$elemMatch: {$gt: 5, $lt: 10}}}");

    // Nested
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // NOLINT
        R"({
            "REDACT_a": {
                "$elemMatch": {
                    "$elemMatch": {
                        "$gt": "?number",
                        "$lt": "?number"
                    }
                }
            }
        })",
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
                    "$centerSphere": "?array<>"
                },
                "field": "REDACT_a"
            }
        })",
        query);
}

TEST(QueryPredicateShape, NorMatchExpression) {
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // NOLINT
        R"({"$nor":[{"REDACT_a":{"$lt":"?number"}},{"REDACT_b":{"$gt":"?number"}}]})",
        "{ $nor: [ { a: {$lt: 5} }, { b: {$gt: 4} } ] }");
}

TEST(QueryPredicateShape, NotMatchExpression) {
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // NOLINT
        R"({"REDACT_price":{"$not":{"$gt":"?number"}}})",
        "{ price: { $not: { $gt: 1.99 } } }");
    // Test the special case where NotMatchExpression::serialize() reduces to $alwaysFalse.
    auto emptyAnd = std::make_unique<AndMatchExpression>();
    const MatchExpression& notExpr = NotMatchExpression(std::move(emptyAnd));
    auto serialized = notExpr.serialize(literalAndFieldRedactOpts);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$alwaysFalse":"?number"})",
        serialized);
}

TEST(QueryPredicateShape, SizeMatchExpression) {
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // NOLINT
        R"({"REDACT_price":{"$size":"?number"}})",
        "{ price: { $size: 2 } }");
}

TEST(QueryPredicateShape, TextMatchExpression) {
    TextMatchExpressionBase::TextParams params = {"coffee"};
    auto expr = ExtensionsCallbackNoop().createText(params);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$text": {
                "$search": "?string",
                "$language": "?string",
                "$caseSensitive": "?bool",
                "$diacriticSensitive": "?bool"
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
        R"({"$where":"?javascript"})",
        "{$where: \"some_code()\"}");
}

BSONObj queryShapeForOptimizedExprExpression(std::string exprPredicateJson) {
    ParsedMatchExpressionForTest expr(exprPredicateJson);
    // We need to optimize an $expr expression in order to generate an $_internalExprEq. It's not
    // clear we'd want to do optimization before computing the query shape, but we should support
    // the computation on any MatchExpression, and this is the easiest way we can create this type
    // of MatchExpression node.
    auto optimized = MatchExpression::optimize(expr.release());
    return query_shape::debugPredicateShape(optimized.get());
}

TEST(QueryPredicateShape, OptimizedExprPredicates) {
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$and": [
                {
                    "a": {
                        "$_internalExprEq": "?number"
                    }
                },
                {
                    "$expr": {
                        "$eq": [
                            "$a",
                            "?number"
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
                        "$_internalExprLt": "?number"
                    }
                },
                {
                    "$expr": {
                        "$lt": [
                            "$a",
                            "?number"
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
                        "$_internalExprLte": "?number"
                    }
                },
                {
                    "$expr": {
                        "$lte": [
                            "$a",
                            "?number"
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
                        "$_internalExprGt": "?number"
                    }
                },
                {
                    "$expr": {
                        "$gt": [
                            "$a",
                            "?number"
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
                        "$_internalExprGte": "?number"
                    }
                },
                {
                    "$expr": {
                        "$gte": [
                            "$a",
                            "?number"
                        ]
                    }
                }
            ]
        })",
        queryShapeForOptimizedExprExpression("{$expr: {$gte: ['$a', 2]}}"));
}

TEST(SortPatternShape, NormalSortPattern) {
    boost::intrusive_ptr<ExpressionContext> expCtx;
    expCtx = make_intrusive<ExpressionContextForTest>();
    SerializationOptions opts;
    opts.replacementForLiteralArgs = query_shape::kLiteralArgString;
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"a.b.c":1,"foo":-1})",
        query_shape::extractSortShape(fromjson(R"({"a.b.c": 1, "foo": -1})"), expCtx, opts));
}

TEST(SortPatternShape, NaturalSortPattern) {
    boost::intrusive_ptr<ExpressionContext> expCtx;
    expCtx = make_intrusive<ExpressionContextForTest>();
    SerializationOptions opts;
    opts.replacementForLiteralArgs = query_shape::kLiteralArgString;
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({$natural: 1})",
        query_shape::extractSortShape(fromjson(R"({$natural: 1})"), expCtx, opts));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({$natural: -1})",
        query_shape::extractSortShape(fromjson(R"({$natural: -1})"), expCtx, opts));
}

TEST(SortPatternShape, NaturalSortPatternWithMeta) {
    boost::intrusive_ptr<ExpressionContext> expCtx;
    expCtx = make_intrusive<ExpressionContextForTest>();
    SerializationOptions opts;
    opts.replacementForLiteralArgs = query_shape::kLiteralArgString;
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({$natural: 1, x: '?'})",
        query_shape::extractSortShape(
            fromjson(R"({$natural: 1, x: {$meta: "textScore"}})"), expCtx, opts));
}

TEST(SortPatternShape, MetaPatternWithoutNatural) {
    boost::intrusive_ptr<ExpressionContext> expCtx;
    expCtx = make_intrusive<ExpressionContextForTest>();
    SerializationOptions opts;
    opts.replacementForLiteralArgs = query_shape::kLiteralArgString;
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"normal":1,"$computed1":{"$meta":"textScore"}})",
        query_shape::extractSortShape(
            fromjson(R"({normal: 1, x: {$meta: "textScore"}})"), expCtx, opts));
}

// Here we have one test to ensure that the redaction policy is accepted and applied in the
// query_shape utility, but there are more extensive redaction tests in sort_pattern_test.cpp
TEST(SortPatternShape, RespectsRedactionPolicy) {
    boost::intrusive_ptr<ExpressionContext> expCtx;
    expCtx = make_intrusive<ExpressionContextForTest>();
    SerializationOptions opts;
    opts.replacementForLiteralArgs = query_shape::kLiteralArgString;
    opts.applyHmacToIdentifiers = true;
    opts.identifierHmacPolicy = applyHmacForTest;
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"REDACT_normal":1,"REDACT_y":1})",
        query_shape::extractSortShape(fromjson(R"({normal: 1, y: 1})"), expCtx, opts));

    // No need to redact $natural.
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$natural":1,"REDACT_y":1})",
        query_shape::extractSortShape(fromjson(R"({$natural: 1, y: 1})"), expCtx, opts));
}

TEST(QueryShapeIDL, ShapifyIDLStruct) {
    SerializationOptions options;
    options.applyHmacToIdentifiers = true;
    options.identifierHmacPolicy = [](StringData s) -> std::string {
        return str::stream() << "HASH<" << s << ">";
    };
    options.replacementForLiteralArgs = "?"_sd;

    auto nested = NestedStruct("value",
                               ExampleEnumEnum::Value1,
                               1337,
                               "hello",
                               {1, 2, 3, 4},
                               "field.path",
                               {"field.path.1", "fieldpath2"},
                               NamespaceString{"db", "coll"},
                               NamespaceString{"db", "coll"});
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "stringField": "value",
            "enumField": "EnumValue1",
            "stringIntVariant": 1337,
            "stringIntVariantEnum": "hello",
            "arrayOfInts": [
                1,
                2,
                3,
                4
            ],
            "fieldpath": "field.path",
            "fieldpathList": [
                "field.path.1",
                "fieldpath2"
            ],
            "nss": "db.coll",
            "plainNss": "db.coll"
        })",
        nested.toBSON());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "stringField": "?",
            "enumField": "EnumValue1",
            "stringIntVariant": "?",
            "stringIntVariantEnum": "hello",
            "arrayOfInts": "?",
            "fieldpath": "HASH<field>.HASH<path>",
            "fieldpathList": [
                "HASH<field>.HASH<path>.HASH<1>",
                "HASH<fieldpath2>"
            ],
            "nss": "HASH<db.coll>",
            "plainNss": "db.coll"
        })",
        nested.toBSON(options));


    auto parent = ParentStruct(nested, nested);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "nested_shape": {
                "stringField": "value",
                "enumField": "EnumValue1",
                "stringIntVariant": 1337,
                "stringIntVariantEnum": "hello",
                "arrayOfInts": [
                    1,
                    2,
                    3,
                    4
                ],
                "fieldpath": "field.path",
                "fieldpathList": [
                    "field.path.1",
                    "fieldpath2"
                ],
                "nss": "db.coll",
                "plainNss": "db.coll"
            },
            "nested_no_shape": {
                "stringField": "value",
                "enumField": "EnumValue1",
                "stringIntVariant": 1337,
                "stringIntVariantEnum": "hello",
                "arrayOfInts": [
                    1,
                    2,
                    3,
                    4
                ],
                "fieldpath": "field.path",
                "fieldpathList": [
                    "field.path.1",
                    "fieldpath2"
                ],
                "nss": "db.coll",
                "plainNss": "db.coll"
            }
        })",
        parent.toBSON());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "nested_shape": {
                "stringField": "?",
                "enumField": "EnumValue1",
                "stringIntVariant": "?",
                "stringIntVariantEnum": "hello",
                "arrayOfInts": "?",
                "fieldpath": "HASH<field>.HASH<path>",
                "fieldpathList": [
                    "HASH<field>.HASH<path>.HASH<1>",
                    "HASH<fieldpath2>"
                ],
                "nss": "HASH<db.coll>",
                "plainNss": "db.coll"
            },
            "nested_no_shape": {
                "stringField": "value",
                "enumField": "EnumValue1",
                "stringIntVariant": 1337,
                "stringIntVariantEnum": "hello",
                "arrayOfInts": [
                    1,
                    2,
                    3,
                    4
                ],
                "fieldpath": "field.path",
                "fieldpathList": [
                    "field.path.1",
                    "fieldpath2"
                ],
                "nss": "db.coll",
                "plainNss": "db.coll"
            }
        })",
        parent.toBSON(options));
}

}  // namespace mongo
