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

#include "mongo/db/query/query_shape/query_shape.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_text_base.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/parsed_match_expression_for_test.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"
#include "mongo/db/query/query_shape/query_shape_test_gen.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <variant>

#include <s2cellid.h>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
namespace mongo::query_shape {

namespace {
BSONObj predicateShape(const MatchExpression* expr) {
    return expr->serialize(SerializationOptions::kDebugQueryShapeSerializeOptions);
}
BSONObj predicateShape(std::string filterJson) {
    return predicateShape(ParsedMatchExpressionForTest(filterJson).get());
}

BSONObj predicateShapeRedacted(const MatchExpression* expr) {
    return expr->serialize(SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST);
}
BSONObj predicateShapeRedacted(std::string filterJson) {
    return predicateShapeRedacted(ParsedMatchExpressionForTest(filterJson).get());
}

#define ASSERT_SHAPE_EQ_AUTO(expected, actual) \
    ASSERT_BSONOBJ_EQ_AUTO(expected, predicateShape(actual))

#define ASSERT_REDACTED_SHAPE_EQ_AUTO(expected, actual) \
    ASSERT_BSONOBJ_EQ_AUTO(expected, predicateShapeRedacted(actual))


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
        R"({"HASH<a>":{"$eq":"?number"}})",
        "{a: 5}");
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // Explicit equals
        R"({"HASH<a>":{"$eq":"?number"}})",
        "{a: {$eq: 5}}");
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // NOLINT
        R"({"$and":[{"HASH<a>":{"$eq":"?number"}},{"HASH<b>":{"$eq":"?number"}}]})",
        "{a: 5, b: 6}");
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // NOLINT
        R"({"HASH<foo>.HASH<$bar>":{"$eq":"?number"}})",
        R"({"foo.$bar":0})");
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
    ASSERT_BSONOBJ_EQ(expectedShape, predicateShape(filterJson));
}

void assertRedactedShapeIs(std::string filterJson, BSONObj expectedShape) {
    ASSERT_BSONOBJ_EQ(expectedShape, predicateShapeRedacted(filterJson));
}
}  // namespace

TEST(QueryPredicateShape, Regex) {
    // Note/warning: 'fromjson' will parse $regex into a /regex/, so these tests can't use
    // auto-updating BSON assertions.
    assertShapeIs("{a: /a+/}", BSON("a" << BSON("$regex" << "?string")));
    assertShapeIs("{a: /a+/i}",
                  BSON("a" << BSON("$regex" << "?string"
                                            << "$options"
                                            << "?string")));
    assertRedactedShapeIs("{a: /a+/}", BSON("HASH<a>" << BSON("$regex" << "?string")));
    assertRedactedShapeIs("{a: /a+/}", BSON("HASH<a>" << BSON("$regex" << "?string")));
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
    // Any number of children in any order is always the same shape
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$in":"?array<?number>"}})",
        "{a: {$in: [1]}}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$in":"?array<>"}})",
        "{a: {$in: [1, 4, 'str', /regex/]}}");
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"a":{"$in":"?array<>"}})",
        "{a: {$in: ['str', /regex/, 1, 4]}}");
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
            "HASH<a>": {
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
        "{ $_internalBucketGeoWithin: {withinRegion: {$centerSphere: [[0, 0], 10]}, field: "
        "\"a\"} "
        "}";
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // NOLINT
        R"({
            "$_internalBucketGeoWithin": {
                "withinRegion": {
                    "$centerSphere": "?array<>"
                },
                "field": "HASH<a>"
            }
        })",
        query);
}

TEST(QueryPredicateShape, NorMatchExpression) {
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // NOLINT
        R"({"$nor":[{"HASH<a>":{"$lt":"?number"}},{"HASH<b>":{"$gt":"?number"}}]})",
        "{ $nor: [ { a: {$lt: 5} }, { b: {$gt: 4} } ] }");
}

TEST(QueryPredicateShape, NotMatchExpression) {
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // NOLINT
        R"({"HASH<price>":{"$not":{"$gt":"?number"}}})",
        "{ price: { $not: { $gt: 1.99 } } }");
    // Test the special case where NotMatchExpression::serialize() reduces to $alwaysFalse.
    auto emptyAnd = std::make_unique<AndMatchExpression>();
    const MatchExpression& notExpr = NotMatchExpression(std::move(emptyAnd));
    auto serialized =
        notExpr.serialize(SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$alwaysFalse":"?number"})",
        serialized);
}

TEST(QueryPredicateShape, SizeMatchExpression) {
    ASSERT_REDACTED_SHAPE_EQ_AUTO(  // NOLINT
        R"({"HASH<price>":{"$size":"?number"}})",
        "{ price: { $size: 2 } }");
}

TEST(QueryPredicateShape, TextMatchExpression) {
    TextMatchExpressionBase::TextParams params = {"coffee"};
    auto expr = ExtensionsCallbackNoop().createText(params);
    auto literalAndFieldRedactOpts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$text": {
                "$search": "?string",
                "$language": "?string",
                "$caseSensitive": "?bool",
                "$diacriticSensitive": "?bool"
            }
        })",
        expr->serialize(SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST));
}

TEST(QueryPredicateShape, TwoDPtInAnnulusExpression) {
    const MatchExpression& expr = TwoDPtInAnnulusExpression({}, {});
    auto literalAndFieldRedactOpts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$TwoDPtInAnnulusExpression":true})",
        expr.serialize(SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST));
}

TEST(QueryPredicateShape, WhereMatchExpression) {
    ASSERT_SHAPE_EQ_AUTO(  // NOLINT
        R"({"$where":"?javascript"})",
        "{$where: \"some_code()\"}");
}

BSONObj queryShapeForOptimizedExprExpression(std::string exprPredicateJson) {
    ParsedMatchExpressionForTest expr(exprPredicateJson);
    // We need to optimize an $expr expression in order to generate an $_internalExprEq. It's
    // not clear we'd want to do optimization before computing the query shape, but we should
    // support the computation on any MatchExpression, and this is the easiest way we can create
    // this type of MatchExpression node.
    auto optimized = optimizeMatchExpression(expr.release());
    return predicateShape(optimized.get());
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

TEST(QueryShapeIDL, ShapifyIDLStruct) {
    SerializationOptions options;
    options.transformIdentifiers = true;
    options.transformIdentifiersCallback = [](StringData s) -> std::string {
        return str::stream() << "HASH<" << s << ">";
    };
    options.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;

    auto nested = NestedStruct("value",
                               ExampleEnumEnum::Value1,
                               1337,
                               "hello",
                               {1, 2, 3, 4},
                               "field.path",
                               {"field.path.1", "fieldpath2"},
                               NamespaceString::createNamespaceString_forTest("db", "coll"),
                               NamespaceString::createNamespaceString_forTest("db", "coll"),
                               177,
                               true);
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
            "plainNss": "db.coll",
            "safeInt64Field": 177,
            "boolField": true
        })",
        nested.toBSON());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "stringField": "?string",
            "enumField": "EnumValue1",
            "stringIntVariant": "?number",
            "stringIntVariantEnum": "hello",
            "arrayOfInts": "?array<?number>",
            "fieldpath": "HASH<field>.HASH<path>",
            "fieldpathList": [
                "HASH<field>.HASH<path>.HASH<1>",
                "HASH<fieldpath2>"
            ],
            "nss": "HASH<db.coll>",
            "plainNss": "db.coll",
            "safeInt64Field": "?number",
            "boolField": "?bool"
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
                "plainNss": "db.coll",
                "safeInt64Field": 177,
                "boolField": true
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
                "plainNss": "db.coll",
                "safeInt64Field": 177,
                "boolField": true
            }
        })",
        parent.toBSON());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "nested_shape": {
                "stringField": "?string",
                "enumField": "EnumValue1",
                "stringIntVariant": "?number",
                "stringIntVariantEnum": "hello",
                "arrayOfInts": "?array<?number>",
                "fieldpath": "HASH<field>.HASH<path>",
                "fieldpathList": [
                    "HASH<field>.HASH<path>.HASH<1>",
                    "HASH<fieldpath2>"
                ],
                "nss": "HASH<db.coll>",
                "plainNss": "db.coll",
                "safeInt64Field": "?number",
                "boolField": "?bool"
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
                "plainNss": "db.coll",
                "safeInt64Field": 177,
                "boolField": true
            }
        })",
        parent.toBSON(options));
}

}  // namespace

namespace {

static const NamespaceString kDefaultTestNss =
    NamespaceString::createNamespaceString_forTest("testDB.testColl");

struct DummyShapeSpecificComponents : public query_shape::CmdSpecificShapeComponents {
    DummyShapeSpecificComponents() {};
    void HashValue(absl::HashState state) const override {}
    size_t size() const final {
        return sizeof(DummyShapeSpecificComponents);
    }
};

class DummyShape : public Shape {
public:
    DummyShape(NamespaceStringOrUUID nssOrUUID,
               BSONObj collation,
               DummyShapeSpecificComponents dummyComponents)
        : Shape(nssOrUUID, collation) {
        components = dummyComponents;
    }

    const CmdSpecificShapeComponents& specificComponents() const final {
        return components;
    }

    void appendCmdSpecificShapeComponents(BSONObjBuilder&,
                                          OperationContext*,
                                          const SerializationOptions& opts) const final {}
    DummyShapeSpecificComponents components;
};

class DummyShapeWithExtraSize : public Shape {
public:
    DummyShapeWithExtraSize(NamespaceStringOrUUID nssOrUUID,
                            BSONObj collation,
                            DummyShapeSpecificComponents dummyComponents)
        : Shape(nssOrUUID, collation) {
        components = dummyComponents;
    }

    const CmdSpecificShapeComponents& specificComponents() const final {
        return components;
    }

    // Random number for testing purposes.
    size_t extraSize() const final {
        return 125;
    }
    void appendCmdSpecificShapeComponents(BSONObjBuilder&,
                                          OperationContext*,
                                          const SerializationOptions& opts) const final {}

    DummyShapeSpecificComponents components;
};

class UniversalShapeTest : public ServiceContextTest {};

TEST_F(UniversalShapeTest, SizeOfSpecificComponents) {
    auto innerComponents = std::make_unique<DummyShapeSpecificComponents>();
    ASSERT_EQ(innerComponents->size(), sizeof(CmdSpecificShapeComponents));
    ASSERT_EQ(innerComponents->size(), sizeof(void*) /*vtable ptr*/);
}

TEST_F(UniversalShapeTest, SizeOfShape) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    // Make shape for testing.
    auto collation = BSONObj{};
    auto innerComponents = std::make_unique<DummyShapeSpecificComponents>();
    auto shape = std::make_unique<DummyShape>(kDefaultTestNss, collation, *innerComponents);

    ASSERT_EQ(innerComponents->size(), shape->specificComponents().size());
    ASSERT_EQ(shape->size(),
              sizeof(NamespaceStringOrUUID) + sizeof(BSONObj) + sizeof(void*) /*vtable ptr*/ +
                  shape->specificComponents().size() + static_cast<size_t>(collation.objsize()));
}

TEST_F(UniversalShapeTest, SizeOfShapeWithExtraSize) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    // Make shape for testing.
    auto collation = BSONObj{};
    auto innerComponents = std::make_unique<DummyShapeSpecificComponents>();
    auto shape = std::make_unique<DummyShape>(kDefaultTestNss, collation, *innerComponents);
    auto shapeWithExtraSize =
        std::make_unique<DummyShapeWithExtraSize>(kDefaultTestNss, collation, *innerComponents);

    ASSERT_EQ(shapeWithExtraSize->size(), shape->size() + shapeWithExtraSize->extraSize());
}
}  // namespace
}  // namespace mongo::query_shape
