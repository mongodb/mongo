// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
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

TEST(JSONSchemaParserScalarTest, MaximumTranslatesCorrectlyWithTypeNumber) {
    BSONObj schema = fromjson("{properties: {num: {type: 'number', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult,
                         fromjson("{$or: [{num: {$not: {$exists: true}}}, {$and: [{num: {$lte: "
                                  "0}}, {num: {$_internalSchemaType: ['number']}}]}]}"));
}

TEST(JSONSchemaParserScalarTest, MaximumTranslatesCorrectlyWithBsonTypeLong) {
    BSONObj schema =
        fromjson("{properties: {num: {bsonType: 'long', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult,
                         fromjson("{$or: [{num: {$not: {$exists: true}}}, {$and: [{num: {$lte: "
                                  "0}}, {num: {$_internalSchemaType: [18]}}]}]}"));
}

TEST(JSONSchemaParserScalarTest, MaximumTranslatesCorrectlyWithTypeString) {
    BSONObj schema = fromjson("{properties: {num: {type: 'string', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(
        optimizedResult,
        fromjson("{$or: [{num: {$not: {$exists: true }}}, {num: {$_internalSchemaType: [2]}}]}"));
}

TEST(JSONSchemaParserScalarTest, MaximumTranslatesCorrectlyWithNoType) {
    BSONObj schema = fromjson("{properties: {num: {maximum: 0}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{num: {$not: {$exists : true}}},
                     {num: {$not: {$_internalSchemaType: ["number"]}}},
                     {num: {$lte: 0}}]
                })"));
}

TEST(JSONSchemaParserScalarTest, FailsToParseIfMaximumIsNotANumber) {
    BSONObj schema = fromjson("{maximum: 'foo'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserScalarTest, FailsToParseIfMaxLengthIsNotANumber) {
    BSONObj schema = fromjson("{maxLength: 'foo'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserScalarTest, FailsToParseIfMaxLengthIsLessThanZero) {
    BSONObj schema = fromjson("{maxLength: -1}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserScalarTest, MinimumTranslatesCorrectlyWithTypeNumber) {
    BSONObj schema = fromjson("{properties: {num: {type: 'number', minimum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{num: {$not: {$exists : true}}},
                     {$and: [ {num: {$gte: 0}}, {num: {$_internalSchemaType: ["number"]}}]}]
                })"));
}

TEST(JSONSchemaParserScalarTest, FailsToParseIfMaxLengthIsNonIntegralDouble) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', maxLength: 5.5}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserScalarTest, MaxLengthTranslatesCorrectlyWithIntegralDouble) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', maxLength: 5.0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{foo: {$not: {$exists: true}}},
                     {$and: [ {foo: {$_internalSchemaMaxLength: 5}}, {foo: {$_internalSchemaType: [2]}}]}]
                })"));
}

TEST(JSONSchemaParserScalarTest, MaxLengthTranslatesCorrectlyWithTypeString) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', maxLength: 5}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{foo: {$not: {$exists : true}}},
                     {$and: [ {foo: {$_internalSchemaMaxLength: 5}}, {foo: {$_internalSchemaType: [2]}}]}]
                })"));
}

TEST(JSONSchemaParserScalarTest, MinimumTranslatesCorrectlyWithBsonTypeLong) {
    BSONObj schema =
        fromjson("{properties: {num: {bsonType: 'long', minimum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{num: {$not: {$exists: true}}},
                     {$and: [ {num: {$gte: 0}}, {num: {$_internalSchemaType: [18]}}]}]
                })"));
}

TEST(JSONSchemaParserScalarTest, MinimumTranslatesCorrectlyWithTypeString) {
    BSONObj schema = fromjson("{properties: {num: {type: 'string', minimum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{num: {$not: {$exists: true}}}, {num: {$_internalSchemaType: [2]}}]
                })"));
}


TEST(JSONSchemaParserScalarTest, MinimumTranslatesCorrectlyWithNoType) {
    BSONObj schema = fromjson("{properties: {num: {minimum: 0}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{num: {$not: {$exists: true}}},
                     {num: {$not: {$_internalSchemaType: ["number"]}}},
                     {num: {$gte: 0}}]
                })"));
}

TEST(JSONSchemaParserScalarTest, MaximumTranslatesCorrectlyWithExclusiveMaximumTrue) {
    BSONObj schema = fromjson(
        "{properties: {num: {bsonType: 'long', maximum: 0, exclusiveMaximum: true}},"
        "type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{num: {$not: {$exists: true}}},
                     {$and: [ {num: {$lt: 0}}, {num: {$_internalSchemaType: [18]}}]}]
                })"));
}

TEST(JSONSchemaParserScalarTest, MaximumTranslatesCorrectlyWithExclusiveMaximumFalse) {
    BSONObj schema = fromjson(
        "{properties: {num: {bsonType: 'long', maximum: 0, exclusiveMaximum: false}},"
        "type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{num: {$not: {$exists: true}}},
                     {$and: [ {num: {$lte: 0}}, {num: {$_internalSchemaType: [18]}}]}]
                })"));
}

TEST(JSONSchemaParserScalarTest, FailsToParseIfExclusiveMaximumIsPresentButMaximumIsNot) {
    BSONObj schema = fromjson("{exclusiveMaximum: true}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserScalarTest, FailsToParseIfExclusiveMaximumIsNotABoolean) {
    BSONObj schema = fromjson("{maximum: 5, exclusiveMaximum: 'foo'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserScalarTest, MinimumTranslatesCorrectlyWithExclusiveMinimumTrue) {
    BSONObj schema = fromjson(
        "{properties: {num: {bsonType: 'long', minimum: 0, exclusiveMinimum: true}},"
        "type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{num: {$not: {$exists: true}}},
                     {$and: [ {num: {$gt: 0}}, {num: {$_internalSchemaType: [18]}}]}]
                })"));
}

TEST(JSONSchemaParserScalarTest, MinimumTranslatesCorrectlyWithExclusiveMinimumFalse) {
    BSONObj schema = fromjson(
        "{properties: {num: {bsonType: 'long', minimum: 0, exclusiveMinimum: false}},"
        "type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{num: {$not: {$exists: true}}},
                     {$and: [ {num: {$gte: 0}}, {num: {$_internalSchemaType: [18]}}]}]
                })"));
}

TEST(JSONSchemaParserScalarTest, FailsToParseIfExclusiveMinimumIsPresentButMinimumIsNot) {
    BSONObj schema = fromjson("{exclusiveMinimum: true}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserScalarTest, FailsToParseIfExclusiveMinimumIsNotABoolean) {
    BSONObj schema = fromjson("{minimum: 5, exclusiveMinimum: 'foo'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserScalarTest, FailsToParseIfMinLengthIsNotANumber) {
    BSONObj schema = fromjson("{minLength: 'foo'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserScalarTest, FailsToParseIfMinLengthIsLessThanZero) {
    BSONObj schema = fromjson("{minLength: -1}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserScalarTest, FailsToParseIfMinLengthIsNonIntegralDouble) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', minLength: 5.5}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserScalarTest, MinLengthTranslatesCorrectlyWithTypeString) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', minLength: 5}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{foo: {$not: {$exists: true}}},
                     {$and: [ {foo: {$_internalSchemaMinLength: 5}}, {foo: {$_internalSchemaType: [2]}}]}]
                })"));
}

TEST(JSONSchemaParserScalarTest, MinLengthTranslatesCorrectlyWithIntegralDouble) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', minLength: 5.0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{foo: {$not: {$exists: true}}},
                     {$and: [ {foo: {$_internalSchemaMinLength: 5}}, {foo: {$_internalSchemaType: [2]}}]}]
                })"));
}

TEST(JSONSchemaParserScalarTest, FailsToParseIfMinimumIsNotANumber) {
    BSONObj schema = fromjson("{minimum: 'foo'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserScalarTest, FailsToParseIfPatternIsNotString) {
    BSONObj schema = fromjson("{pattern: 6}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserScalarTest, PatternTranslatesCorrectlyWithString) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', pattern: 'abc'}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    auto expected =
        BSON("$or" << BSON_ARRAY(
                 BSON("foo" << BSON("$not" << BSON("$exists" << true)))
                 << BSON("$and" << BSON_ARRAY(
                             BSON("foo" << BSON("$regex" << "abc"))
                             << BSON("foo" << BSON("$_internalSchemaType" << BSON_ARRAY(2)))))));
    ASSERT_SERIALIZES_TO(optimizedResult, expected);
}

TEST(JSONSchemaParserScalarTest, FailsToParseIfMultipleOfIsNotANumber) {
    BSONObj schema = fromjson("{multipleOf: 'foo'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserScalarTest, FailsToParseIfMultipleOfIsLessThanZero) {
    BSONObj schema = fromjson("{multipleOf: -1}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserScalarTest, FailsToParseIfMultipleOfIsZero) {
    BSONObj schema = fromjson("{multipleOf: 0}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserScalarTest, MultipleOfTranslatesCorrectlyWithTypeNumber) {
    BSONObj schema = fromjson(
        "{properties: {foo: {type: 'number', multipleOf: NumberDecimal('5.3')}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{foo: {$not: {$exists: true}}}, {
                        $and: [
                            {foo: {$_internalSchemaFmod: [ NumberDecimal('5.3'), 0]}},
                            {foo: {$_internalSchemaType: ["number"]}}
                        ]
                    }]
                })"));
}

}  // namespace
}  // namespace mongo
