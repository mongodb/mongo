// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/schema/assert_serializes_to.h"
#include "mongo/db/query/compiler/parsers/matcher/schema/json_schema_parser.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <utility>

namespace mongo {
namespace {

TEST(JSONSchemaLogicalKeywordTest, FailsToParseIfAllOfIsNotAnArray) {
    BSONObj schema = fromjson("{properties: {foo: {allOf: 'foo'}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaLogicalKeywordTest, FailsToParseAllOfIfArrayContainsInvalidSchema) {
    BSONObj schema = fromjson("{properties: {foo: {allOf: [{type: {}}]}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaLogicalKeywordTest, FailsToParseAllOfIfArrayIsEmpty) {
    BSONObj schema = fromjson("{properties: {foo: {allOf: []}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
}

TEST(JSONSchemaLogicalKeywordTest, AllOfTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {foo: {allOf: [{minimum: 0}, {maximum: 10}]}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    auto expectedResult = fromjson(
        R"({
            $or:
                [{foo: {$not: {$exists: true}}},
                 {
                   $and : [
                       {$or: [ {foo: {$not: {$_internalSchemaType: ["number"]}}}, {foo: {$gte: 0}}]},
                       {$or: [ {foo: {$not: {$_internalSchemaType: ["number"]}}}, {foo: {$lte: 10}}]}
                   ]
                 }]
            })");
    ASSERT_SERIALIZES_TO(optimizedResult, expectedResult);
}

TEST(JSONSchemaLogicalKeywordTest, TopLevelAllOfTranslatesCorrectly) {
    BSONObj schema = fromjson("{allOf: [{properties: {foo: {type: 'string'}}}]}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{foo: {$not: {$exists: true}}}, {foo: {$_internalSchemaType: [2]}}]
                })"));
}

TEST(JSONSchemaLogicalKeywordTest, FailsToParseIfAnyOfIsNotAnArray) {
    BSONObj schema = fromjson("{properties: {foo: {anyOf: 'foo'}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaLogicalKeywordTest, FailsToParseAnyOfIfArrayContainsInvalidSchema) {
    BSONObj schema = fromjson("{properties: {foo: {anyOf: [{type: {}}]}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaLogicalKeywordTest, FailsToParseAnyOfIfArrayIsEmpty) {
    BSONObj schema = fromjson("{properties: {foo: {anyOf: []}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
}

TEST(JSONSchemaLogicalKeywordTest, AnyOfTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {foo: {anyOf: [{type: 'number'}, {type: 'string'}]}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{foo: {$not: {$exists: true}}},
                     {foo: {$_internalSchemaType: ["number"]}},
                     {foo: {$_internalSchemaType: [2]}}]
                })"));
}

TEST(JSONSchemaLogicalKeywordTest, TopLevelAnyOfTranslatesCorrectly) {
    BSONObj schema = fromjson("{anyOf: [{properties: {foo: {type: 'string'}}}]}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{foo: {$not: {$exists: true}}}, {foo: {$_internalSchemaType: [2]}}]
                })"));
}

TEST(JSONSchemaLogicalKeywordTest, FailsToParseIfOneOfIsNotAnArray) {
    BSONObj schema = fromjson("{properties: {foo: {oneOf: 'foo'}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaLogicalKeywordTest, FailsToParseOneOfIfArrayContainsInvalidSchema) {
    BSONObj schema = fromjson("{properties: {foo: {oneOf: [{type: {}}]}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaLogicalKeywordTest, FailsToParseOneOfIfArrayIsEmpty) {
    BSONObj schema = fromjson("{properties: {foo: {oneOf: []}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
}

TEST(JSONSchemaLogicalKeywordTest, OneOfTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {foo: {oneOf: [{minimum: 0}, {maximum: 10}]}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $or:
            [{foo: {$not: {$exists: true}}},
             {
               $_internalSchemaXor : [
                   {$or: [ {foo: {$not: {$_internalSchemaType: ["number"]}}}, {foo: {$gte : 0}}]},
                   {$or: [ {foo: {$not: {$_internalSchemaType: ["number"]}}}, {foo: {$lte : 10}}]}
               ]
             }]
        })"));
}

TEST(JSONSchemaLogicalKeywordTest, TopLevelOneOfTranslatesCorrectly) {
    BSONObj schema = fromjson("{oneOf: [{properties: {foo: {type: 'string'}}}]}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{foo: {$not: {$exists: true}}}, {foo: {$_internalSchemaType: [2]}}]
                })"));
}

TEST(JSONSchemaLogicalKeywordTest, FailsToParseIfNotIsNotAnObject) {
    BSONObj schema = fromjson("{properties: {foo: {not: 'foo'}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaLogicalKeywordTest, FailsToParseNotIfObjectContainsInvalidSchema) {
    BSONObj schema = fromjson("{properties: {foo: {not: {type: {}}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaLogicalKeywordTest, NotTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {foo: {not: {type: 'number'}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{foo: {$not: {$exists: true}}}, {foo: {$not: {$_internalSchemaType: ['number']}}}]
                })"));
}

TEST(JSONSchemaLogicalKeywordTest, TopLevelNotTranslatesCorrectly) {
    BSONObj schema = fromjson("{not: {properties: {foo: {type: 'string'}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $nor:
            [{$or: [ {foo: {$not: {$exists: true}}}, {foo: {$_internalSchemaType: [2]}}]}]
        })"));
}

TEST(JSONSchemaLogicalKeywordTest, FailsToParseIfEnumIsNotAnArray) {
    BSONObj schema = fromjson("{properties: {foo: {enum: 'foo'}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaLogicalKeywordTest, FailsToParseEnumIfArrayIsEmpty) {
    BSONObj schema = fromjson("{properties: {foo: {enum: []}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaLogicalKeywordTest, FailsToParseEnumIfArrayContainsDuplicateValue) {
    BSONObj schema = fromjson("{properties: {foo: {enum: [1, 2, 1]}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);

    schema = fromjson("{properties: {foo: {enum: [{a: 1, b: 1}, {b: 1, a: 1}]}}}");
    result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaLogicalKeywordTest, EnumTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {foo: {enum: [1, '2', [3]]}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $or:
            [{foo: {$not: {$exists: true}}},
             {foo: {$_internalSchemaEq: 1}},
             {foo: {$_internalSchemaEq: "2"}},
             {foo: {$_internalSchemaEq: [3]}}]
        })"));
}

TEST(JSONSchemaLogicalKeywordTest, TopLevelEnumTranslatesCorrectly) {
    BSONObj schema = fromjson("{enum: [1, {foo: 1}]}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{$_internalSchemaRootDocEq: {foo: 1}}"));
}

TEST(JSONSchemaLogicalKeywordTest, TopLevelEnumWithZeroObjectsTranslatesCorrectly) {
    BSONObj schema = fromjson("{enum: [1, 'impossible', true]}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{$alwaysFalse: 1}"));
}

}  // namespace
}  // namespace mongo
