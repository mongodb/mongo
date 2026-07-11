// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// Unit tests for MatchExpression::serialize serialization.


// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"
#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

using namespace std::literals::string_view_literals;

BSONObj serialize(MatchExpression* match) {
    return match->serialize();
}

TEST(SerializeBasic, ExpressionOrWithNoChildrenSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // We construct an OrMatchExpression directly rather than using the match expression
    // parser, since the parser does not permit a $or with no children.
    OrMatchExpression original;
    Matcher reserialized(serialize(&original),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), BSON(AlwaysFalseMatchExpression::kName << 1));
}

TEST(SerializeBasic, ExpressionJsonSchemaWithDollarFieldSerializesShapeCorrectly) {
    auto query = fromjson(R"({$jsonSchema: {properties: {$stdDevPop: {type: 'array'}}}})");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    query_shape::SerializationOptions opts;
    opts.literalPolicy = query_shape::LiteralSerializationPolicy::kToRepresentativeParseableValue;

    // Serialization is correct upon the first parse.
    auto serialized = objMatch.getValue()->serialize(opts);
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "$and":[{
                "$and":[{
                    "$or":[
                        {"$nor": [{"$_internalPath": {"$stdDevPop":{"$exists": true}}}]},
                        {"$and":[{"$_internalPath":{"$stdDevPop":{"$_internalSchemaType":[4]}}}]}
                    ]
                }]
            }]
        })",
        serialized);

    // Confirm that round trip serialization (reparsing) works correctly for query stats.
    auto roundTrip = MatchExpressionParser::parse(serialized, expCtx).getValue()->serialize(opts);
    ASSERT_BSONOBJ_EQ(roundTrip, serialized);
}

TEST(SerializeBasic, ExpressionJsonSchemaWithKeywordDollarFieldSerializesShapeCorrectly) {
    auto query = fromjson(R"({$jsonSchema: {properties: {$or: {type: 'array'}}}})");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    query_shape::SerializationOptions opts;
    opts.literalPolicy = query_shape::LiteralSerializationPolicy::kToRepresentativeParseableValue;

    // Serialization is correct upon the first parse.
    auto serialized = objMatch.getValue()->serialize(opts);
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "$and":[{
                "$and":[{
                    "$or":[
                        {"$nor": [{"$_internalPath": {"$or":{"$exists": true}}}]},
                        {"$and":[{"$_internalPath":{"$or":{"$_internalSchemaType":[4]}}}]}
                    ]
                }]
            }]
        })",
        serialized);

    // Confirm that round trip serialization (reparsing) works correctly for query stats.
    auto roundTrip = MatchExpressionParser::parse(serialized, expCtx).getValue()->serialize(opts);
    ASSERT_BSONOBJ_EQ(roundTrip, serialized);
}

TEST(SerializeBasic, NonLeafDollarPrefixedPathSerializesShapeCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    auto baseOperandVal = BSON("$gt" << 5);
    auto gt = std::make_unique<GTMatchExpression>(""sv, baseOperandVal["$gt"]);
    auto elemMatchValExpr = std::make_unique<ElemMatchValueMatchExpression>("$a"sv);
    elemMatchValExpr->add(std::move(gt));

    query_shape::SerializationOptions opts;
    opts.literalPolicy = query_shape::LiteralSerializationPolicy::kToRepresentativeParseableValue;

    // Serialization is correct upon the first parse.
    BSONObjBuilder bob;
    elemMatchValExpr->serialize(&bob, opts);
    auto serialized = bob.obj();
    ASSERT_BSONOBJ_EQ_AUTO(R"({"$_internalPath": {"$a":{"$elemMatch": {"$gt": 1}}}})", serialized);

    // Confirm that round trip serialization (reparsing) works correctly for query stats.
    auto roundTrip = MatchExpressionParser::parse(serialized, expCtx).getValue()->serialize(opts);
    ASSERT_BSONOBJ_EQ(roundTrip, serialized);
}

TEST(SerializeBasic, ExpressionRegexWithoutOptionsSerializesShapeCorrectly) {
    auto query = fromjson(R"({x: {$regex: ".*"}})");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());
    query_shape::SerializationOptions opts;
    opts.literalPolicy = query_shape::LiteralSerializationPolicy::kToRepresentativeParseableValue;
    ASSERT_BSONOBJ_EQ_AUTO(R"({"x":{"$regex":"\\?"}})", objMatch.getValue()->serialize(opts));
}

TEST(SerializeBasic, ExpressionRegexWithOptionsSerializesShapeCorrectly) {
    auto query = fromjson(R"({x: {$regex: ".*", $options: "m"}})");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());
    query_shape::SerializationOptions opts;
    opts.literalPolicy = query_shape::LiteralSerializationPolicy::kToRepresentativeParseableValue;
    ASSERT_BSONOBJ_EQ_AUTO(R"({"x":{"$regex":"\\?","$options":"i"}})",
                           objMatch.getValue()->serialize(opts));
}

TEST(SerializeBasic, ExpressionWhereSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$where: 'this.a == this.b'}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      BSONObjBuilder().appendCode("$where", "this.a == this.b").obj());
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionNearSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(
        fromjson("{x: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}, $maxDistance: 10, "
                 "$minDistance: 1}}}"),
        expCtx,
        ExtensionsCallbackNoop(),
        MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(
        *reserialized.getQuery(),
        fromjson("{x: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}, $maxDistance: 10, "
                 "$minDistance: 1}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionNearSphereSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(
        fromjson(
            "{x: {$nearSphere: {$geometry: {type: 'Point', coordinates: [0, 0]}, $maxDistance: 10, "
            "$minDistance: 1}}}"),
        expCtx,
        ExtensionsCallbackNoop(),
        MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(
        *reserialized.getQuery(),
        fromjson("{x: {$nearSphere: {$geometry: {type: 'Point', coordinates: [0, 0]}, "
                 "$maxDistance: 10, $minDistance: 1}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionTextSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$text: {$search: 'a', $language: 'en', $caseSensitive: true}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{$text: {$search: 'a', $language: 'en', $caseSensitive: true, "
                               "$diacriticSensitive: false}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionNorWithTextSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$nor: [{$text: {$search: 'x'}}]}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{$nor: [{$text: {$search: 'x', $language: '', $caseSensitive: "
                               "false, $diacriticSensitive: false}}]}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionTextWithDefaultLanguageSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$text: {$search: 'a', $caseSensitive: false}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{$text: {$search: 'a', $language: '', $caseSensitive: false, "
                               "$diacriticSensitive: false}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionAlwaysTrueSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(BSON(AlwaysTrueMatchExpression::kName << 1),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), BSON(AlwaysTrueMatchExpression::kName << 1));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionAlwaysFalseSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(BSON(AlwaysFalseMatchExpression::kName << 1),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), BSON(AlwaysFalseMatchExpression::kName << 1));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaAllElemMatchFromIndexSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaAllElemMatchFromIndex: [2, {y: 1}]}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{x: {$_internalSchemaAllElemMatchFromIndex: [2, {y: {$eq: 1}}]}}"));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaMinItemsSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaMinItems: 1}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$_internalSchemaMinItems: 1}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaMaxItemsSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaMaxItems: 1}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$_internalSchemaMaxItems: 1}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaUniqueItemsSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaUniqueItems: true}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{x: {$_internalSchemaUniqueItems: true}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaObjectMatchSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaObjectMatch: {y: 1}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{x: {$_internalSchemaObjectMatch: {y: {$eq: 1}}}}"));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaMinLengthSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaMinLength: 1}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$_internalSchemaMinLength: 1}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaMaxLengthSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaMaxLength: 1}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$_internalSchemaMaxLength: 1}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaCondSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$_internalSchemaCond: [{a: 1}, {b: 2}, {c: 3}]}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    BSONObjBuilder builder;
    ASSERT_BSONOBJ_EQ(
        *reserialized.getQuery(),
        fromjson("{$_internalSchemaCond: [{a: {$eq: 1}}, {b: {$eq: 2}}, {c: {$eq: 3}}]}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaMinPropertiesSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$_internalSchemaMinProperties: 1}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{$_internalSchemaMinProperties: 1}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaMaxPropertiesSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$_internalSchemaMaxProperties: 1}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{$_internalSchemaMaxProperties: 1}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaMatchArrayIndexSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{a: {$_internalSchemaMatchArrayIndex:"
                              "{index: 2, namePlaceholder: 'i', expression: {i: {$lt: 3}}}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{a: {$_internalSchemaMatchArrayIndex:"
                               "{index: 2, namePlaceholder: 'i', expression: {i: {$lt: 3}}}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaAllowedPropertiesSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson(R"({$_internalSchemaAllowedProperties: {
            properties: ['a'],
            otherwise: {i: {$gt: 10}},
            namePlaceholder: 'i',
            patternProperties: [{regex: /b/, expression: {i: {$type: 'number'}}}]
        }})"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson(R"({$_internalSchemaAllowedProperties: {
            properties: ['a'],
            namePlaceholder: 'i',
            patternProperties: [{regex: /b/, expression: {i: {$type: ['number']}}}],
            otherwise: {i: {$gt: 10}}
        }})"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema,
     ExpressionInternalSchemaAllowedPropertiesEmptyOtherwiseSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson(R"({$_internalSchemaAllowedProperties: {
            properties: [],
            otherwise: {},
            namePlaceholder: 'i',
            patternProperties: []
        }})"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson(R"({$_internalSchemaAllowedProperties: {
            properties: [],
            namePlaceholder: 'i',
            patternProperties: [],
            otherwise: {}
        }})"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaEqSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaEq: {y: 1}}}"), expCtx);
    Matcher reserialized(serialize(original.getMatchExpression()), expCtx);

    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$_internalSchemaEq: {y: 1}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaRootDocEqSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$_internalSchemaRootDocEq: {y: 1}}"), expCtx);
    Matcher reserialized(serialize(original.getMatchExpression()), expCtx);

    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{$_internalSchemaRootDocEq: {y: 1}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, AllowedPropertiesRedactsCorrectly) {

    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: ['a', 'b'],"
        "namePlaceholder: 'i', patternProperties: [], otherwise: {i: 0}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    query_shape::SerializationOptions opts =
        query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalSchemaAllowedProperties": {
                "properties": "?array<?string>",
                "namePlaceholder": "i",
                "patternProperties": [],
                "otherwise": {
                    "HASH<i>": {
                        "$eq": "?number"
                    }
                }
            }
        })",
        objMatch.getValue()->serialize(opts));
}

/**
 * Helper function for parsing and creating MatchExpressions.
 */
std::unique_ptr<InternalSchemaCondMatchExpression> createCondMatchExpression(BSONObj condition,
                                                                             BSONObj thenBranch,
                                                                             BSONObj elseBranch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto conditionExpr = MatchExpressionParser::parse(condition, expCtx);
    ASSERT_OK(conditionExpr.getStatus());
    auto thenBranchExpr = MatchExpressionParser::parse(thenBranch, expCtx);
    ASSERT_OK(thenBranchExpr.getStatus());
    auto elseBranchExpr = MatchExpressionParser::parse(elseBranch, expCtx);

    std::array<std::unique_ptr<MatchExpression>, 3> expressions = {
        {std::move(conditionExpr.getValue()),
         std::move(thenBranchExpr.getValue()),
         std::move(elseBranchExpr.getValue())}};

    auto cond = std::make_unique<InternalSchemaCondMatchExpression>(std::move(expressions));

    return cond;
}

TEST(SerializeInternalSchema, CondMatchRedactsCorrectly) {
    auto opts = query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    auto conditionQuery = BSON("age" << BSON("$lt" << 18));
    auto thenQuery = BSON("job" << "student");
    auto elseQuery = BSON("job" << "engineer");
    auto cond = createCondMatchExpression(conditionQuery, thenQuery, elseQuery);
    BSONObjBuilder bob;
    cond->serialize(&bob, opts);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalSchemaCond": [
                {
                    "HASH<age>": {
                        "$lt": "?number"
                    }
                },
                {
                    "HASH<job>": {
                        "$eq": "?string"
                    }
                },
                {
                    "HASH<job>": {
                        "$eq": "?string"
                    }
                }
            ]
        })",
        bob.done());
}

TEST(SerializeInternalSchema, FmodMatchRedactsCorrectly) {
    InternalSchemaFmodMatchExpression m("a"sv, Decimal128(1.7), Decimal128(2));
    auto opts = query_shape::SerializationOptions{
        query_shape::LiteralSerializationPolicy::kToDebugTypeString};
    BSONObjBuilder bob;
    m.serialize(&bob, opts);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"a":{"$_internalSchemaFmod":["?number","?number"]}})",
        bob.done());
}

TEST(SerializeInternalSchema, MatchArrayIndexRedactsCorrectly) {
    auto query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {i: {$type: 'number'}}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    BSONObjBuilder bob;
    auto opts = query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    objMatch.getValue()->serialize(&bob, opts);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "HASH<foo>": {
                "$_internalSchemaMatchArrayIndex": {
                    "index": "?number",
                    "namePlaceholder": "HASH<i>",
                    "expression": {
                        "HASH<i>": {
                            "$type": ['number']
                        }
                    }
                }
            }
        })",
        bob.done());
}

TEST(SerializeInternalSchema, MaxItemsRedactsCorrectly) {
    InternalSchemaMaxItemsMatchExpression maxItems("a.b"sv, 2);
    auto opts = query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalSchemaMaxItems":"?number"})",
        maxItems.getSerializedRightHandSide(opts));
}

TEST(SerializeInternalSchema, MaxLengthRedactsCorrectly) {
    InternalSchemaMaxLengthMatchExpression maxLength("a"sv, 2);
    auto opts = query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalSchemaMaxLength":"?number"})",
        maxLength.getSerializedRightHandSide(opts));
}

TEST(SerializeInternalSchema, MinItemsRedactsCorrectly) {
    InternalSchemaMinItemsMatchExpression minItems("a.b"sv, 2);
    auto opts = query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalSchemaMinItems":"?number"})",
        minItems.getSerializedRightHandSide(opts));
}

TEST(SerializeInternalSchema, MinLengthRedactsCorrectly) {
    InternalSchemaMinLengthMatchExpression minLength("a"sv, 2);
    auto opts = query_shape::SerializationOptions{
        query_shape::LiteralSerializationPolicy::kToDebugTypeString};
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalSchemaMinLength":"?number"})",
        minLength.getSerializedRightHandSide(opts));
}

TEST(SerializeInternalSchema, MinPropertiesRedactsCorrectly) {
    InternalSchemaMinPropertiesMatchExpression minProperties(5);
    auto opts = query_shape::SerializationOptions{
        query_shape::LiteralSerializationPolicy::kToDebugTypeString};

    BSONObjBuilder bob;
    minProperties.serialize(&bob, opts);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalSchemaMinProperties":"?number"})",
        bob.done());
}

TEST(SerializeInternalSchema, ObjectMatchRedactsCorrectly) {
    auto opts = query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    auto query = fromjson(
        "    {a: {$_internalSchemaObjectMatch: {"
        "        c: {$eq: 3}"
        "    }}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"HASH<a>":{"$_internalSchemaObjectMatch":{"HASH<c>":{"$eq":"?number"}}}})",
        objMatch.getValue()->serialize(opts));
}

TEST(SerializeInternalSchema, RootDocEqRedactsCorrectly) {
    auto query = fromjson("{$_internalSchemaRootDocEq: {a:1, b: {c: 1, d: [1]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto opts = query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalSchemaRootDocEq": {
                "HASH<a>": "?number",
                "HASH<b>": {
                    "HASH<c>": "?number",
                    "HASH<d>": [
                        "?number"
                    ]
                }
            }
        })",
        objMatch.getValue()->serialize(opts));
}

TEST(SerializeInternalSchema, BinDataEncryptedTypeRedactsCorrectly) {
    MatcherTypeSet typeSet;
    typeSet.bsonTypes.insert(BSONType::string);
    typeSet.bsonTypes.insert(BSONType::date);
    InternalSchemaBinDataEncryptedTypeExpression e("a"sv, std::move(typeSet));
    auto opts = query_shape::SerializationOptions{
        query_shape::LiteralSerializationPolicy::kToDebugTypeString};
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalSchemaBinDataEncryptedType":[2,9]})",
        e.getSerializedRightHandSide(opts));
}

TEST(SerializeInternalSchema, BinDataFLE2EncryptedTypeRedactsCorrectly) {
    InternalSchemaBinDataFLE2EncryptedTypeExpression e("ssn"sv, BSONType::string);
    auto opts = query_shape::SerializationOptions{
        query_shape::LiteralSerializationPolicy::kToDebugTypeString};
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalSchemaBinDataFLE2EncryptedType":[2]})",
        e.getSerializedRightHandSide(opts));
}

TEST(SerializesInternalSchema, MaxPropertiesRedactsCorrectly) {
    InternalSchemaMaxPropertiesMatchExpression maxProperties(5);
    auto opts = query_shape::SerializationOptions{
        query_shape::LiteralSerializationPolicy::kToDebugTypeString};

    BSONObjBuilder bob;
    maxProperties.serialize(&bob, opts);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalSchemaMaxProperties":"?number"})",
        bob.done());
}

TEST(SerializesInternalSchema, EqRedactsCorrectly) {
    auto opts = query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    auto query = fromjson("{$_internalSchemaEq: {a:1, b: {c: 1, d: [1]}}}");
    BSONObjBuilder bob;
    InternalSchemaEqMatchExpression e("a"sv, query.firstElement());
    e.serialize(&bob, opts);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "HASH<a>": {
                "$_internalSchemaEq": {
                    "HASH<a>": "?number",
                    "HASH<b>": {
                        "HASH<c>": "?number",
                        "HASH<d>": [
                            "?number"
                        ]
                    }
                }
            }
        })",
        bob.done());
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, RedactsExpressionCorrectly) {
    auto query = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [2, {a: {$lt: 5}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());
    auto elemMatchExpr = dynamic_cast<const InternalSchemaAllElemMatchFromIndexMatchExpression*>(
        expr.getValue().get());

    auto opts = query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalSchemaAllElemMatchFromIndex": [
                "?number",
                {
                    "HASH<a>": {
                        "$lt": "?number"
                    }
                }
            ]
        })",
        elemMatchExpr->getSerializedRightHandSide(opts));
}

TEST(SerializeBasic, SerializesNestedElemMatchCorrectly) {
    auto query = fromjson(R"({a: {$elemMatch: {$elemMatch: {b: {$lt: 6, $gt: 4}}}}})");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());
    query_shape::SerializationOptions opts;
    opts.literalPolicy = query_shape::LiteralSerializationPolicy::kToDebugTypeString;
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({"a": {
                "$elemMatch": {
                    "$elemMatch": {
                        "$and": [
                            {
                                "b": {
                                    "$lt": "?number"
                                }
                            },
                            {
                                "b": {
                                    "$gt": "?number"
                                }
                            }
                        ]
                    }
                }
            }
        })",
        objMatch.getValue()->serialize(opts));
}
}  // namespace
}  // namespace mongo
