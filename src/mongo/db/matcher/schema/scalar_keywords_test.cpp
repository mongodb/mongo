/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/schema/json_schema_parser.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

#define ASSERT_SERIALIZES_TO(match, expected)   \
    do {                                        \
        BSONObjBuilder bob;                     \
        match->serialize(&bob);                 \
        ASSERT_BSONOBJ_EQ(bob.obj(), expected); \
    } while (false)

TEST(JSONSchemaParserScalarTest, MaximumTranslatesCorrectlyWithTypeNumber) {
    BSONObj schema = fromjson("{properties: {num: {type: 'number', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult,
                         fromjson("{$or: [{num: {$not: {$exists: true}}}, {$and: [{num: {$lte: "
                                  "0}}, {num: {$_internalSchemaType: ['number']}}]}]}"));
}

TEST(JSONSchemaParserScalarTest, MaximumTranslatesCorrectlyWithBsonTypeLong) {
    BSONObj schema =
        fromjson("{properties: {num: {bsonType: 'long', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult,
                         fromjson("{$or: [{num: {$not: {$exists: true}}}, {$and: [{num: {$lte: "
                                  "0}}, {num: {$_internalSchemaType: [18]}}]}]}"));
}

TEST(JSONSchemaParserScalarTest, MaximumTranslatesCorrectlyWithTypeString) {
    BSONObj schema = fromjson("{properties: {num: {type: 'string', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(
        optimizedResult,
        fromjson("{$or: [{num: {$not: {$exists: true }}}, {num: {$_internalSchemaType: [2]}}]}"));
}

TEST(JSONSchemaParserScalarTest, MaximumTranslatesCorrectlyWithNoType) {
    BSONObj schema = fromjson("{properties: {num: {maximum: 0}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{num: {$not: {$exists: true}}}, {num: {$_internalSchemaType: [2]}}]
                })"));
}


TEST(JSONSchemaParserScalarTest, MinimumTranslatesCorrectlyWithNoType) {
    BSONObj schema = fromjson("{properties: {num: {minimum: 0}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    auto expected =
        BSON("$or" << BSON_ARRAY(
                 BSON("foo" << BSON("$not" << BSON("$exists" << true)))
                 << BSON("$and" << BSON_ARRAY(
                             BSON("foo" << BSON("$regex"
                                                << "abc"))
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
