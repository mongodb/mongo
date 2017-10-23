/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/db/bson/bson_helper.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/schema/json_schema_parser.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

#define ASSERT_SERIALIZES_TO(match, expected)   \
    do {                                        \
        BSONObjBuilder bob;                     \
        match->serialize(&bob);                 \
        ASSERT_BSONOBJ_EQ(bob.obj(), expected); \
    } while (false)

TEST(JSONSchemaParserTest, FailsToParseIfTypeIsNotAString) {
    BSONObj schema = fromjson("{type: 1}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseNicelyIfTypeIsKnownUnsupportedAlias) {
    BSONObj schema = fromjson("{type: 'integer'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema type 'integer' is not currently supported");
}

TEST(JSONSchemaParserTest, FailsToParseUnknownKeyword) {
    BSONObj schema = fromjson("{unknown: 1}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, FailsToParseIfPropertiesIsNotAnObject) {
    BSONObj schema = fromjson("{properties: 1}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseIfPropertiesIsNotAnObjectWithType) {
    BSONObj schema = fromjson("{type: 'string', properties: 1}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseIfParticularPropertyIsNotAnObject) {
    BSONObj schema = fromjson("{properties: {foo: 1}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseIfKeywordIsDuplicated) {
    BSONObj schema = BSON("type"
                          << "object"
                          << "type"
                          << "object");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, EmptySchemaTranslatesCorrectly) {
    BSONObj schema = fromjson("{}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{}"));
}

TEST(JSONSchemaParserTest, TypeObjectTranslatesCorrectly) {
    BSONObj schema = fromjson("{type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{}"));
}

TEST(JSONSchemaParserTest, NestedTypeObjectTranslatesCorrectly) {
    BSONObj schema =
        fromjson("{properties: {a: {type: 'object', properties: {b: {type: 'string'}}}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{a: {$exists: true}}]},
                                {
                                    $and: [
                                        {
                                            a: {
                                                $_internalSchemaObjectMatch: {
                                                    $or: [
                                                        {$nor: [{b: {$exists: true}}]},
                                                        {b: {$_internalSchemaType: [2]}}
                                                    ]
                                                }
                                            }
                                        },
                                        {a: {$_internalSchemaType: [3]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, TopLevelNonObjectTypeTranslatesCorrectly) {
    BSONObj schema = fromjson("{type: 'string'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, BSON(AlwaysFalseMatchExpression::kName << 1));
}

TEST(JSONSchemaParserTest, TypeNumberTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {num: {type: 'number'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{num: {$exists: true}}]},
                                {num: {$_internalSchemaType: ['number']}}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithTypeNumber) {
    BSONObj schema = fromjson("{properties: {num: {type: 'number', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{num: {$exists: true}}]},
                                {
                                    $and: [
                                        {num: {$lte: 0 }},
                                        {num: {$_internalSchemaType: ['number']}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithBsonTypeLong) {
    BSONObj schema =
        fromjson("{properties: {num: {bsonType: 'long', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{num: {$exists: true}}]},
                                {
                                    $and: [
                                        {num: {$lte: 0}},
                                        {num: {$_internalSchemaType: [18]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithTypeString) {
    BSONObj schema = fromjson("{properties: {num: {type: 'string', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{num: {$exists: true}}]},
                                {
                                    $and: [
                                        {$alwaysTrue: 1},
                                        {num: {$_internalSchemaType: [2]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithNoType) {
    BSONObj schema = fromjson("{properties: {num: {maximum: 0}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $or: [
            {$nor: [{num: {$exists: true}}]},
            {$nor: [{ num: {$_internalSchemaType: ['number']}}]},
            {num: {$lte: 0}}]
        })"));
}

TEST(JSONSchemaParserTest, FailsToParseIfMaximumIsNotANumber) {
    BSONObj schema = fromjson("{maximum: 'foo'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseIfMaxLengthIsNotANumber) {
    BSONObj schema = fromjson("{maxLength: 'foo'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, FailsToParseIfMaxLengthIsLessThanZero) {
    BSONObj schema = fromjson("{maxLength: -1}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, MinimumTranslatesCorrectlyWithTypeNumber) {
    BSONObj schema = fromjson("{properties: {num: {type: 'number', minimum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{num: {$exists: true}}]},
                                {
                                    $and: [
                                        {num: {$gte: 0}},
                                        {num: {$_internalSchemaType: ['number']}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, FailsToParseIfMaxLengthIsNonIntegralDouble) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', maxLength: 5.5}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, MaxLengthTranslatesCorrectlyWithIntegralDouble) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', maxLength: 5.0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{foo: {$exists: true}}]},
                                {
                                    $and: [
                                        {foo: {$_internalSchemaMaxLength: 5}},
                                        {foo: {$_internalSchemaType: [2]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, MaxLengthTranslatesCorrectlyWithTypeString) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', maxLength: 5}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {
                                    $nor: [{foo: {$exists: true}}]
                                },
                                {
                                    $and: [
                                        {foo: {$_internalSchemaMaxLength: 5}},
                                        {foo: { $_internalSchemaType: [2]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, MinimumTranslatesCorrectlyWithBsonTypeLong) {
    BSONObj schema =
        fromjson("{properties: {num: {bsonType: 'long', minimum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{num: {$exists: true}}]},
                                {
                                    $and: [
                                        {num: {$gte: 0}},
                                        { num: { $_internalSchemaType: [18]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, MinimumTranslatesCorrectlyWithTypeString) {
    BSONObj schema = fromjson("{properties: {num: {type: 'string', minimum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{num: {$exists: true}}]},
                                {
                                    $and: [
                                        {$alwaysTrue: 1},
                                        {num: {$_internalSchemaType: [2]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, MinimumTranslatesCorrectlyWithNoType) {
    BSONObj schema = fromjson("{properties: {num: {minimum: 0}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{num: {$exists: true}}]},
                                {$nor: [{num: {$_internalSchemaType: ['number']}}]},
                                {num: {$gte: 0}}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithExclusiveMaximumTrue) {
    BSONObj schema = fromjson(
        "{properties: {num: {bsonType: 'long', maximum: 0, exclusiveMaximum: true}},"
        "type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{num: {$exists: true}}]},
                                {
                                    $and: [
                                        {num: {$lt: 0}},
                                        {num: {$_internalSchemaType: [18]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithExclusiveMaximumFalse) {
    BSONObj schema = fromjson(
        "{properties: {num: {bsonType: 'long', maximum: 0, exclusiveMaximum: false}},"
        "type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{num: {$exists: true}}]},
                                {
                                    $and: [
                                        {num: {$lte: 0}},
                                        {num: {$_internalSchemaType: [18]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, FailsToParseIfExclusiveMaximumIsPresentButMaximumIsNot) {
    BSONObj schema = fromjson("{exclusiveMaximum: true}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, FailsToParseIfExclusiveMaximumIsNotABoolean) {
    BSONObj schema = fromjson("{maximum: 5, exclusiveMaximum: 'foo'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, MinimumTranslatesCorrectlyWithExclusiveMinimumTrue) {
    BSONObj schema = fromjson(
        "{properties: {num: {bsonType: 'long', minimum: 0, exclusiveMinimum: true}},"
        "type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{num: {$exists: true}}]},
                                {
                                    $and: [
                                        {num: {$gt: 0}},
                                        {num: {$_internalSchemaType: [18]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, MinimumTranslatesCorrectlyWithExclusiveMinimumFalse) {
    BSONObj schema = fromjson(
        "{properties: {num: {bsonType: 'long', minimum: 0, exclusiveMinimum: false}},"
        "type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{num: {$exists: true}}]},
                                {
                                    $and: [
                                        {num: {$gte: 0}},
                                        {num: {$_internalSchemaType: [18]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, FailsToParseIfExclusiveMinimumIsPresentButMinimumIsNot) {
    BSONObj schema = fromjson("{exclusiveMinimum: true}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, FailsToParseIfExclusiveMinimumIsNotABoolean) {
    BSONObj schema = fromjson("{minimum: 5, exclusiveMinimum: 'foo'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseIfMinLengthIsNotANumber) {
    BSONObj schema = fromjson("{minLength: 'foo'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, FailsToParseIfMinLengthIsLessThanZero) {
    BSONObj schema = fromjson("{minLength: -1}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, FailsToParseIfMinLengthIsNonIntegralDouble) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', minLength: 5.5}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, MinLengthTranslatesCorrectlyWithTypeString) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', minLength: 5}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{foo: {$exists: true}}]},
                                {
                                    $and: [
                                        {foo: {$_internalSchemaMinLength: 5}},
                                        {foo: {$_internalSchemaType: [2]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, MinLengthTranslatesCorrectlyWithIntegralDouble) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', minLength: 5.0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{foo: {$exists: true}}]},
                                {
                                    $and: [
                                        {foo: {$_internalSchemaMinLength: 5}},
                                        {foo: {$_internalSchemaType: [2]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, FailsToParseIfMinimumIsNotANumber) {
    BSONObj schema = fromjson("{minimum: 'foo'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseIfPatternIsNotString) {
    BSONObj schema = fromjson("{pattern: 6}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, PatternTranslatesCorrectlyWithString) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', pattern: 'abc'}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    BSONObj expected =
        BSON("$or" << BSON_ARRAY(
                 BSON("$nor" << BSON_ARRAY(BSON("foo" << BSON("$exists" << true))))
                 << BSON("$and" << BSON_ARRAY(
                             BSON("foo" << BSON("$regex"
                                                << "abc"))
                             << BSON("foo" << BSON("$_internalSchemaType" << BSON_ARRAY(2)))))));
    ASSERT_SERIALIZES_TO(optimizedResult, expected);
}

TEST(JSONSchemaParserTest, FailsToParseIfMultipleOfIsNotANumber) {
    BSONObj schema = fromjson("{multipleOf: 'foo'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseIfMultipleOfIsLessThanZero) {
    BSONObj schema = fromjson("{multipleOf: -1}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, FailsToParseIfMultipleOfIsZero) {
    BSONObj schema = fromjson("{multipleOf: 0}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, MultipleOfTranslatesCorrectlyWithTypeNumber) {
    BSONObj schema = fromjson(
        "{properties: {foo: {type: 'number', multipleOf: NumberDecimal('5.3')}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{foo: {$exists: true}}]},
                                {
                                    $and: [
                                        {foo: {$_internalSchemaFmod: [NumberDecimal('5.3'), 0]}},
                                        {foo: {$_internalSchemaType: ['number']}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, FailsToParseIfAllOfIsNotAnArray) {
    BSONObj schema = fromjson("{properties: {foo: {allOf: 'foo'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseAllOfIfArrayContainsInvalidSchema) {
    BSONObj schema = fromjson("{properties: {foo: {allOf: [{type: {}}]}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseAllOfIfArrayIsEmpty) {
    BSONObj schema = fromjson("{properties: {foo: {allOf: []}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
}

TEST(JSONSchemaParserTest, AllOfTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {foo: {allOf: [{minimum: 0}, {maximum: 10}]}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    auto expectedResult = fromjson(
        R"({
            $or: [
                {$nor: [{foo: {$exists: true}}]},
                {
                    $and: [
                        {
                            $or: [
                                {$nor: [{foo:{ $_internalSchemaType: ['number']}}]},
                                {foo: {$gte: 0}}
                            ]
                        },
                        {
                            $or: [
                                {$nor: [{foo: {$_internalSchemaType: ['number']}}]},
                                {foo: {$lte: 10}}
                            ]
                        }
                    ]
                }
            ]
        })");
    ASSERT_SERIALIZES_TO(optimizedResult, expectedResult);
}

TEST(JSONSchemaParserTest, TopLevelAllOfTranslatesCorrectly) {
    BSONObj schema = fromjson("{allOf: [{properties: {foo: {type: 'string'}}}]}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{foo: {$exists: true}}]},
                                {foo: {$_internalSchemaType: [2]}}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, FailsToParseIfAnyOfIsNotAnArray) {
    BSONObj schema = fromjson("{properties: {foo: {anyOf: 'foo'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseAnyOfIfArrayContainsInvalidSchema) {
    BSONObj schema = fromjson("{properties: {foo: {anyOf: [{type: {}}]}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseAnyOfIfArrayIsEmpty) {
    BSONObj schema = fromjson("{properties: {foo: {anyOf: []}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
}

TEST(JSONSchemaParserTest, AnyOfTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {foo: {anyOf: [{type: 'number'}, {type: 'string'}]}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{foo: {$exists: true}}]},
                                {foo: {$_internalSchemaType: ['number']}},
                                {foo: {$_internalSchemaType: [2]}}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, TopLevelAnyOfTranslatesCorrectly) {
    BSONObj schema = fromjson("{anyOf: [{properties: {foo: {type: 'string'}}}]}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{foo: {$exists: true}}]},
                                {foo: {$_internalSchemaType: [2]}}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, FailsToParseIfOneOfIsNotAnArray) {
    BSONObj schema = fromjson("{properties: {foo: {oneOf: 'foo'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseOneOfIfArrayContainsInvalidSchema) {
    BSONObj schema = fromjson("{properties: {foo: {oneOf: [{type: {}}]}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseOneOfIfArrayIsEmpty) {
    BSONObj schema = fromjson("{properties: {foo: {oneOf: []}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
}

TEST(JSONSchemaParserTest, OneOfTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {foo: {oneOf: [{minimum: 0}, {maximum: 10}]}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{foo: {$exists: true}}]},
                                {
                                    $_internalSchemaXor: [
                                        {
                                            $or: [
                                                {$nor: [{foo: {$_internalSchemaType: ['number']}}]},
                                                {foo: {$gte: 0}}
                                            ]
                                        },
                                        {
                                            $or: [
                                                {$nor: [{foo: {$_internalSchemaType: ['number']}}]},
                                                {foo: {$lte: 10}}
                                            ]
                                        }
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, TopLevelOneOfTranslatesCorrectly) {
    BSONObj schema = fromjson("{oneOf: [{properties: {foo: {type: 'string'}}}]}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{foo: {$exists: true}}]},
                                {foo: {$_internalSchemaType: [2]}}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, FailsToParseIfNotIsNotAnObject) {
    BSONObj schema = fromjson("{properties: {foo: {not: 'foo'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseNotIfObjectContainsInvalidSchema) {
    BSONObj schema = fromjson("{properties: {foo: {not: {type: {}}}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, NotTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {foo: {not: {type: 'number'}}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{foo: {$exists: true}}]},
                                {$nor: [{ foo: {$_internalSchemaType: ['number']}}]}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, TopLevelNotTranslatesCorrectly) {
    BSONObj schema = fromjson("{not: {properties: {foo: {type: 'string'}}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $nor: [
                                {
                                    $or: [
                                        {$nor: [{foo: {$exists: true}}]},
                                        {foo: {$_internalSchemaType: [2]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, FailsToParseIfMinItemsIsNotANumber) {
    auto schema = BSON("minItems" << BSON_ARRAY(1));
    ASSERT_EQ(JSONSchemaParser::parse(schema).getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, FailsToParseIfMinItemsIsNotANonNegativeInteger) {
    auto schema = BSON("minItems" << -1);
    ASSERT_EQ(JSONSchemaParser::parse(schema).getStatus(), ErrorCodes::FailedToParse);

    schema = BSON("minItems" << 3.14);
    ASSERT_EQ(JSONSchemaParser::parse(schema).getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, MinItemsTranslatesCorrectlyWithNoType) {
    auto schema = BSON("minItems" << 1);
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{$alwaysTrue: 1}"));

    schema = fromjson("{properties: {a: {minItems: 1}}}");
    result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{a: {$exists: true}}]},
                                {$nor: [{a: {$_internalSchemaType: [4]}}]},
                                {a: {$_internalSchemaMinItems: 1}}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, MinItemsTranslatesCorrectlyWithArrayType) {
    auto schema = fromjson("{properties: {a: {minItems: 1, type: 'array'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{a: {$exists: true}}]},
                                {
                                    $and: [
                                        {a: {$_internalSchemaMinItems: 1}},
                                        {a: {$_internalSchemaType: [4]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, MinItemsTranslatesCorrectlyWithNonArrayType) {
    auto schema = fromjson("{properties: {a: {minItems: 1, type: 'number'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{a: {$exists: true}}]},
                                {
                                    $and: [
                                        {$alwaysTrue: 1},
                                        {a: {$_internalSchemaType: ['number']}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, FailsToParseIfMaxItemsIsNotANumber) {
    auto schema = BSON("maxItems" << BSON_ARRAY(1));
    ASSERT_EQ(JSONSchemaParser::parse(schema).getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, FailsToParseIfMaxItemsIsNotANonNegativeInteger) {
    auto schema = BSON("maxItems" << -1);
    ASSERT_EQ(JSONSchemaParser::parse(schema).getStatus(), ErrorCodes::FailedToParse);

    schema = BSON("maxItems" << 1.60217);
    ASSERT_EQ(JSONSchemaParser::parse(schema).getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, MaxItemsTranslatesCorrectlyWithNoType) {
    auto schema = BSON("maxItems" << 1);
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{$alwaysTrue: 1}"));

    schema = fromjson("{properties: {a: {maxItems: 1}}}");
    result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{a: {$exists: true}}]},
                                {$nor: [{a: {$_internalSchemaType: [4]}}]},
                                {a: {$_internalSchemaMaxItems: 1}}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, MaxItemsTranslatesCorrectlyWithArrayType) {
    auto schema = fromjson("{properties: {a: {maxItems: 1, type: 'array'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{a: {$exists: true}}]},
                                {
                                    $and: [
                                        {a: {$_internalSchemaMaxItems: 1}},
                                        {a: {$_internalSchemaType: [4]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, MaxItemsTranslatesCorrectlyWithNonArrayType) {
    auto schema = fromjson("{properties: {a: {maxItems: 1, type: 'string'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{a: {$exists: true}}]},
                                {
                                    $and: [
                                        {$alwaysTrue: 1},
                                        {a: {$_internalSchemaType: [2]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, RequiredFailsToParseIfNotAnArray) {
    BSONObj schema = fromjson("{required: 'field'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, RequiredFailsToParseArrayIsEmpty) {
    BSONObj schema = fromjson("{required: []}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, RequiredFailsToParseIfArrayContainsNonString) {
    BSONObj schema = fromjson("{required: ['foo', 1]}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, RequiredFailsToParseIfArrayContainsDuplicates) {
    BSONObj schema = fromjson("{required: ['foo', 'bar', 'foo']}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, TopLevelRequiredTranslatesCorrectly) {
    BSONObj schema = fromjson("{required: ['foo', 'bar']}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult,
                         fromjson("{$and: [{bar: {$exists: true}}, {foo: {$exists: true}}]}"));
}

TEST(JSONSchemaParserTest, TopLevelRequiredTranslatesCorrectlyWithProperties) {
    BSONObj schema = fromjson("{required: ['foo'], properties: {foo: {type: 'number'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $and: [
                                {foo: {$_internalSchemaType: ['number']}},
                                {foo: {$exists: true}}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, RequiredTranslatesCorrectlyInsideProperties) {
    BSONObj schema = fromjson("{properties: {x: {required: ['y']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{x: {$exists: true}}]},
                                {$nor: [{x: {$_internalSchemaType: [3]}}]},
                                {x: {$_internalSchemaObjectMatch: {y: {$exists: true }}}}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, RequiredTranslatesCorrectlyInsidePropertiesWithSiblingProperties) {
    BSONObj schema =
        fromjson("{properties: {x:{required: ['y'], properties: {y: {type: 'number'}}}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    auto expectedResult = fromjson(
        R"({
            $or: [
                {$nor: [{x: {$exists: true}}]},
                {
                    $and: [
                        {
                            $or: [
                                {$nor: [{x: {$_internalSchemaType: [3]}}]},
                                {
                                    x: {
                                        $_internalSchemaObjectMatch: {
                                            y: {$_internalSchemaType: ['number']}
                                        }
                                    }
                                }
                            ]
                        },
                        {
                            $or: [
                                {$nor: [{x: {$_internalSchemaType: [3]}}]},
                                {x: {$_internalSchemaObjectMatch: {y: {$exists: true}}}}
                            ]
                        }
                    ]
                }
            ]
        })");
    ASSERT_SERIALIZES_TO(optimizedResult, expectedResult);
}

TEST(JSONSchemaParserTest, SharedJsonAndBsonTypeAliasesTranslateIdentically) {
    for (auto&& mapEntry : MatcherTypeSet::kJsonSchemaTypeAliasMap) {
        auto typeAlias = mapEntry.first;
        // JSON Schema spells its bool type as "boolean", whereas MongoDB calls it "bool".
        auto bsonTypeAlias =
            (typeAlias == JSONSchemaParser::kSchemaTypeBoolean) ? "bool" : typeAlias;

        BSONObj typeSchema = BSON("properties" << BSON("f" << BSON("type" << typeAlias)));
        BSONObj bsonTypeSchema =
            BSON("properties" << BSON("f" << BSON("bsonType" << bsonTypeAlias)));
        auto typeResult = JSONSchemaParser::parse(typeSchema);
        ASSERT_OK(typeResult.getStatus());
        auto bsonTypeResult = JSONSchemaParser::parse(bsonTypeSchema);
        ASSERT_OK(bsonTypeResult.getStatus());

        BSONObjBuilder typeBuilder;
        MatchExpression::optimize(std::move(typeResult.getValue()))->serialize(&typeBuilder);

        BSONObjBuilder bsonTypeBuilder;
        MatchExpression::optimize(std::move(bsonTypeResult.getValue()))
            ->serialize(&bsonTypeBuilder);

        ASSERT_BSONOBJ_EQ(typeBuilder.obj(), bsonTypeBuilder.obj());
    }
}

TEST(JSONSchemaParserTest, MinPropertiesFailsToParseIfNotNumber) {
    BSONObj schema = fromjson("{minProperties: null}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, MaxPropertiesFailsToParseIfNotNumber) {
    BSONObj schema = fromjson("{maxProperties: null}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, MinPropertiesFailsToParseIfNegative) {
    BSONObj schema = fromjson("{minProperties: -2}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, MaxPropertiesFailsToParseIfNegative) {
    BSONObj schema = fromjson("{maxProperties: -2}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, MinPropertiesFailsToParseIfNotAnInteger) {
    BSONObj schema = fromjson("{minProperties: 1.1}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, MaxPropertiesFailsToParseIfNotAnInteger) {
    BSONObj schema = fromjson("{maxProperties: 1.1}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, TopLevelMinPropertiesTranslatesCorrectly) {
    BSONObj schema = fromjson("{minProperties: 0}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{$_internalSchemaMinProperties: 0}"));
}

TEST(JSONSchemaParserTest, TopLevelMaxPropertiesTranslatesCorrectly) {
    BSONObj schema = fromjson("{maxProperties: 0}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{$_internalSchemaMaxProperties: 0}"));
}

TEST(JSONSchemaParserTest, NestedMinPropertiesTranslatesCorrectly) {
    BSONObj schema =
        fromjson("{properties: {obj: {type: 'object', minProperties: 2}}, required: ['obj']}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    auto expectedResult = fromjson(
        R"({
            $and: [
                {obj: {$exists: true}},
                {obj: {$_internalSchemaObjectMatch: {$_internalSchemaMinProperties: 2}}},
                {obj: {$_internalSchemaType: [3]}}
            ]
        })");
    ASSERT_SERIALIZES_TO(optimizedResult, expectedResult);
}

TEST(JSONSchemaParserTest, NestedMaxPropertiesTranslatesCorrectly) {
    BSONObj schema =
        fromjson("{properties: {obj: {type: 'object', maxProperties: 2}}, required: ['obj']}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    auto expectedResult = fromjson(
        R"({
            $and: [
                {obj: {$exists: true}},
                {obj: {$_internalSchemaObjectMatch: {$_internalSchemaMaxProperties: 2}}},
                {obj: {$_internalSchemaType: [3]}}
            ]
        })");
    ASSERT_SERIALIZES_TO(optimizedResult, expectedResult);
}

TEST(JSONSchemaParserTest, NestedMinPropertiesTranslatesCorrectlyWithoutRequired) {
    BSONObj schema = fromjson("{properties: {obj: {type: 'object', minProperties: 2}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    auto expectedResult = fromjson(R"(
        {
            $or: [
                {$nor: [{obj: {$exists: true}}]},
                {
                    $and: [
                        {obj: {$_internalSchemaObjectMatch: {$_internalSchemaMinProperties: 2}}},
                        {obj: {$_internalSchemaType: [3]}}
                    ]
                }
            ]
        })");
    ASSERT_SERIALIZES_TO(optimizedResult, expectedResult);
}

TEST(JSONSchemaParserTest, NestedMaxPropertiesTranslatesCorrectlyWithoutRequired) {
    BSONObj schema = fromjson("{properties: {obj: {type: 'object', maxProperties: 2}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    auto expectedResult = fromjson(R"(
        {
            $or: [
                {$nor: [{obj: {$exists: true}}]},
                {
                    $and: [
                        {obj: {$_internalSchemaObjectMatch: {$_internalSchemaMaxProperties: 2}}},
                        {obj: {$_internalSchemaType: [3]}}
                    ]
                }
            ]
        })");
    ASSERT_SERIALIZES_TO(optimizedResult, expectedResult);
}

TEST(JSONSchemaParserTest, FailsToParseIfTypeArrayHasRepeatedAlias) {
    BSONObj schema = fromjson("{properties: {obj: {type: ['object', 'string', 'object']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, FailsToParseIfBsonTypeArrayHasRepeatedAlias) {
    BSONObj schema = fromjson("{properties: {obj: {bsonType: ['object', 'string', 'object']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, FailsToParseIfTypeArrayIsEmpty) {
    BSONObj schema = fromjson("{properties: {obj: {type: []}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, FailsToParseIfBsonTypeArrayIsEmpty) {
    BSONObj schema = fromjson("{properties: {obj: {bsonType: []}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, FailsToParseIfTypeArrayContainsNonString) {
    BSONObj schema = fromjson("{properties: {obj: {type: [1]}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, FailsToParseIfBsonTypeArrayContainsNonString) {
    BSONObj schema = fromjson("{properties: {obj: {bsonType: [1]}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, FailsToParseIfTypeArrayContainsUnknownAlias) {
    BSONObj schema = fromjson("{properties: {obj: {type: ['objectId']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, FailsToParseNicelyIfTypeArrayContainsKnownUnsupportedAlias) {
    BSONObj schema = fromjson("{properties: {obj: {type: ['number', 'integer']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema type 'integer' is not currently supported");
}

TEST(JSONSchemaParserTest, FailsToParseIfBsonTypeArrayContainsUnknownAlias) {
    BSONObj schema = fromjson("{properties: {obj: {bsonType: ['unknown']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, CanTranslateTopLevelTypeArrayWithoutObject) {
    BSONObj schema = fromjson("{type: ['number', 'string']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue(), BSON(AlwaysFalseMatchExpression::kName << 1));
}

TEST(JSONSchemaParserTest, CanTranslateTopLevelBsonTypeArrayWithoutObject) {
    BSONObj schema = fromjson("{bsonType: ['number', 'string']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue(), BSON(AlwaysFalseMatchExpression::kName << 1));
}

TEST(JSONSchemaParserTest, CanTranslateTopLevelTypeArrayWithObject) {
    BSONObj schema = fromjson("{type: ['number', 'object']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue(), fromjson("{}"));
}

TEST(JSONSchemaParserTest, CanTranslateTopLevelBsonTypeArrayWithObject) {
    BSONObj schema = fromjson("{bsonType: ['number', 'object']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue(), fromjson("{}"));
}

TEST(JSONSchemaParserTest, CanTranslateNestedTypeArray) {
    BSONObj schema = fromjson("{properties: {a: {type: ['number', 'object']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{a: {$exists: true}}]},
                                {a: {$_internalSchemaType: ['number', 3]}}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, CanTranslateNestedBsonTypeArray) {
    BSONObj schema = fromjson("{properties: {a: {bsonType: ['number', 'objectId']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{a: {$exists: true}}]},
                                {a: {$_internalSchemaType: ['number', 7]}}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, DependenciesFailsToParseIfNotAnObject) {
    BSONObj schema = fromjson("{dependencies: []}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, DependenciesFailsToParseIfDependencyIsNotObjectOrArray) {
    BSONObj schema = fromjson("{dependencies: {a: ['b'], bad: 1}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, DependenciesFailsToParseIfNestedSchemaIsInvalid) {
    BSONObj schema = fromjson("{dependencies: {a: {invalid: 1}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, PropertyDependencyFailsToParseIfEmptyArray) {
    BSONObj schema = fromjson("{dependencies: {a: []}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, PropertyDependencyFailsToParseIfArrayContainsNonStringElement) {
    BSONObj schema = fromjson("{dependencies: {a: ['b', 1]}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, PropertyDependencyFailsToParseIfRepeatedArrayElement) {
    BSONObj schema = fromjson("{dependencies: {a: ['b', 'b']}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, TopLevelSchemaDependencyTranslatesCorrectly) {
    BSONObj schema = fromjson("{dependencies: {a: {properties: {b: {type: 'string'}}}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $_internalSchemaCond: [
                                {a: {$exists: true}},
                                {
                                    $or: [
                                        {$nor: [{b: {$exists: true}}]},
                                        {b: {$_internalSchemaType: [2]}}
                                    ]
                                },
                                {$alwaysTrue: 1}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, TopLevelPropertyDependencyTranslatesCorrectly) {
    BSONObj schema = fromjson("{dependencies: {a: ['b', 'c']}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $_internalSchemaCond: [
                                {a: {$exists: true}},
                                {
                                    $and: [
                                        {b: {$exists: true}},
                                        {c: {$exists: true}}
                                    ]
                                },
                                {$alwaysTrue: 1}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, NestedSchemaDependencyTranslatesCorrectly) {
    BSONObj schema =
        fromjson("{properties: {a: {dependencies: {b: {properties: {c: {type: 'object'}}}}}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{a: {$exists: true}}]},
                                {
                                    $_internalSchemaCond: [
                                        {a: {$_internalSchemaObjectMatch: {b: {$exists: true}}}},
                                        {
                                            $or: [
                                                {$nor: [{a: {$_internalSchemaType: [3]}}]},
                                                {
                                                    a: {
                                                        $_internalSchemaObjectMatch: {
                                                            $or: [
                                                                {$nor: [{c: {$exists: true}}]},
                                                                {c: {$_internalSchemaType: [3]}}
                                                            ]
                                                        }
                                                    }
                                                }
                                            ]
                                        },
                                        {$alwaysTrue: 1}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, NestedPropertyDependencyTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {a: {dependencies: {b: ['c', 'd']}}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    auto expectedResult = fromjson(R"(
        {
            $or: [
                {$nor: [{a: {$exists: true}}]},
                {
                    $_internalSchemaCond: [
                        {a: {$_internalSchemaObjectMatch: {b: {$exists: true}}}},
                        {
                            $and: [
                                {a: {$_internalSchemaObjectMatch: {c: {$exists: true}}}},
                                {a: {$_internalSchemaObjectMatch: {d: {$exists: true}}}}
                            ]
                        },
                        {$alwaysTrue: 1}
                    ]
                }
            ]
        })");
    ASSERT_SERIALIZES_TO(optimizedResult, expectedResult);
}

TEST(JSONSchemaParserTest, EmptyDependenciesTranslatesCorrectly) {
    BSONObj schema = fromjson("{dependencies: {}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue(), fromjson("{$and: [{}]}"));
}

TEST(JSONSchemaParserTest, UnsupportedKeywordsFailNicely) {
    auto result = JSONSchemaParser::parse(fromjson("{default: {}}"));
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword 'default' is not currently supported");

    result = JSONSchemaParser::parse(fromjson("{definitions: {numberField: {type: 'number'}}}"));
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword 'definitions' is not currently supported");

    result = JSONSchemaParser::parse(fromjson("{format: 'email'}"));
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword 'format' is not currently supported");

    result = JSONSchemaParser::parse(fromjson("{id: 'someschema.json'}"));
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword 'id' is not currently supported");

    result = JSONSchemaParser::parse(BSON("$ref"
                                          << "#/definitions/positiveInt"));
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword '$ref' is not currently supported");

    result = JSONSchemaParser::parse(fromjson("{$schema: 'hyper-schema'}"));
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword '$schema' is not currently supported");

    result =
        JSONSchemaParser::parse(fromjson("{$schema: 'http://json-schema.org/draft-04/schema#'}"));
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword '$schema' is not currently supported");
}

TEST(JSONSchemaParserTest, FailsToParseIfDescriptionIsNotAString) {
    auto result = JSONSchemaParser::parse(fromjson("{description: {}}"));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, CorrectlyParsesDescriptionAsString) {
    auto result = JSONSchemaParser::parse(fromjson("{description: 'str'}"));
    ASSERT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, CorrectlyParsesNestedDescriptionAsString) {
    auto result = JSONSchemaParser::parse(fromjson("{properties: {a: {description: 'str'}}}"));
    ASSERT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, FailsToParseIfTitleIsNotAString) {
    auto result = JSONSchemaParser::parse(fromjson("{title: {}}"));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, CorrectlyParsesTitleAsString) {
    auto result = JSONSchemaParser::parse(fromjson("{title: 'str'}"));
    ASSERT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, CorrectlyParsesNestedTitleAsString) {
    auto result = JSONSchemaParser::parse(fromjson("{properties: {a: {title: 'str'}}}"));
    ASSERT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, PatternPropertiesFailsToParseIfNotObject) {
    BSONObj schema = fromjson("{patternProperties: 1}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, PatternPropertiesFailsToParseIfOnePropertyIsNotObject) {
    BSONObj schema = fromjson("{patternProperties: {a: {}, b: 1}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, PatternPropertiesFailsToParseIfNestedSchemaIsInvalid) {
    BSONObj schema = fromjson("{patternProperties: {a: {invalid: 1}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, PatternPropertiesFailsToParseIfPropertyNameIsAnInvalidRegex) {
    BSONObj schema = fromjson("{patternProperties: {'[': {}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, AdditionalPropertiesFailsToParseIfNotBoolOrString) {
    BSONObj schema = fromjson("{additionalProperties: 1}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, AdditionalPropertiesFailsToParseIfNestedSchemaIsInvalid) {
    BSONObj schema = fromjson("{additionalProperties: {invalid: 1}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, TopLevelPatternPropertiesTranslatesCorrectly) {
    BSONObj schema =
        fromjson("{patternProperties: {'^a': {type: 'number'}, '^b': {type: 'string'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $_internalSchemaAllowedProperties: {
                                properties: [],
                                namePlaceholder: "i",
                                patternProperties: [
                                    {
                                        regex: /^a/,
                                        expression: {
                                            i: {$_internalSchemaType: ['number']}
                                        }
                                    },
                                    {
                                        regex: /^b/,
                                        expression: {
                                            i: {$_internalSchemaType: [2]}
                                        }
                                    }
                                ],
                                otherwise: {$alwaysTrue: 1}
                            }
                        })"));
}

TEST(JSONSchemaParserTest, TopLevelAdditionalPropertiesFalseTranslatesCorrectly) {
    BSONObj schema = fromjson("{additionalProperties: false}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $_internalSchemaAllowedProperties: {
                                properties: [],
                                namePlaceholder: "i",
                                patternProperties: [],
                                otherwise: {$alwaysFalse: 1}
                            }
                        })"));
}

TEST(JSONSchemaParserTest, TopLevelAdditionalPropertiesTrueTranslatesCorrectly) {
    BSONObj schema = fromjson("{additionalProperties: true}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $_internalSchemaAllowedProperties: {
                                properties: [],
                                namePlaceholder: "i",
                                patternProperties: [],
                                otherwise: {$alwaysTrue: 1}
                            }
                        })"));
}

TEST(JSONSchemaParserTest, TopLevelAdditionalPropertiesTypeNumberTranslatesCorrectly) {
    BSONObj schema = fromjson("{additionalProperties: {type: 'number'}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $_internalSchemaAllowedProperties: {
                                properties: [],
                                namePlaceholder: "i",
                                patternProperties: [],
                                otherwise: {i: {$_internalSchemaType: ['number']}}
                            }
                        })"));
}

TEST(JSONSchemaParserTest, NestedAdditionalPropertiesTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {obj: {additionalProperties: {type: 'number'}}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{obj: {$exists: true}}]},
                                {$nor: [{obj: {$_internalSchemaType: [3]}}]},
                                {
                                    obj: {
                                        $_internalSchemaObjectMatch: {
                                            $_internalSchemaAllowedProperties: {
                                                properties: [],
                                                namePlaceholder: "i",
                                                patternProperties: [],
                                                otherwise: {i: {$_internalSchemaType: ['number']}}
                                            }
                                        }
                                    }
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest,
     PropertiesPatternPropertiesAndAdditionalPropertiesTranslateCorrectlyTogether) {
    BSONObj schema = fromjson(
        "{properties: {a: {}, b: {}}, patternProperties: {'^c': {}}, additionalProperties: false}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $and: [
                                {
                                    $_internalSchemaAllowedProperties: {
                                        properties: ["a", "b"],
                                        namePlaceholder: "i",
                                        patternProperties: [
                                            {regex: /^c/, expression: {}}
                                        ],
                                        otherwise: {$alwaysFalse: 1}
                                    }
                                },
                                {
                                    $or: [
                                        {$nor: [{a: {$exists: true}}]},
                                        {}
                                    ]
                                },
                                {
                                    $or: [
                                        {$nor: [{b: {$exists: true}}]},
                                        {}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest,
     PropertiesPatternPropertiesAdditionalPropertiesAndRequiredTranslateCorrectlyTogether) {
    BSONObj schema = fromjson(
        "{properties: {a: {}, b: {}}, required: ['a'], patternProperties: {'^c': {}}, "
        "additionalProperties: false}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $and: [
                                {
                                    $or: [
                                        {$nor: [{b: {$exists: true}}]},
                                        {}
                                    ]
                                },
                                {
                                    $_internalSchemaAllowedProperties: {
                                        properties: ["a", "b"],
                                        namePlaceholder: "i",
                                        patternProperties: [
                                            {regex: /^c/, expression: {}}
                                        ],
                                        otherwise: {$alwaysFalse: 1}
                                    }
                                },
                                {a: {$exists: true}}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, FailsToParseIfUniqueItemsIsNotABoolean) {
    auto schema = BSON("uniqueItems" << 1);
    ASSERT_EQ(JSONSchemaParser::parse(schema).getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, UniqueItemsFalseGeneratesAlwaysTrueExpression) {
    auto schema = fromjson("{properties: {a: {uniqueItems: false}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{a: {$exists: true}}]},
                                {$alwaysTrue: 1}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, UniqueItemsTranslatesCorrectlyWithNoType) {
    auto schema = BSON("uniqueItems" << true);
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{$alwaysTrue: 1}"));

    schema = fromjson("{properties: {a: {uniqueItems: true}}}");
    result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{a: {$exists: true}}]},
                                {$nor: [{a: {$_internalSchemaType: [4]}}]},
                                {a: {$_internalSchemaUniqueItems: true}}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, UniqueItemsTranslatesCorrectlyWithTypeArray) {
    auto schema = fromjson("{properties: {a: {type: 'array', uniqueItems: true}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{a: {$exists: true}}]},
                                {
                                    $and: [
                                        {a: {$_internalSchemaUniqueItems: true}},
                                        {a: {$_internalSchemaType: [4]}}
                                    ]
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, CorrectlyIgnoresUnknownKeywordsParameterIsSet) {
    const auto ignoreUnknownKeywords = true;

    auto schema = fromjson("{ignored_keyword: 1}");
    ASSERT_OK(JSONSchemaParser::parse(schema, ignoreUnknownKeywords).getStatus());

    schema = fromjson("{properties: {a: {ignored_keyword: 1}}}");
    ASSERT_OK(JSONSchemaParser::parse(schema, ignoreUnknownKeywords).getStatus());

    schema = fromjson("{properties: {a: {oneOf: [{ignored_keyword: {}}]}}}");
    ASSERT_OK(JSONSchemaParser::parse(schema, ignoreUnknownKeywords).getStatus());
}

TEST(JSONSchemaParserTest, FailsToParseUnsupportedKeywordsWhenIgnoreUnknownParameterIsSet) {
    const auto ignoreUnknownKeywords = true;

    auto result = JSONSchemaParser::parse(fromjson("{default: {}}"), ignoreUnknownKeywords);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword 'default' is not currently supported");

    result = JSONSchemaParser::parse(fromjson("{definitions: {numberField: {type: 'number'}}}"),
                                     ignoreUnknownKeywords);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword 'definitions' is not currently supported");

    result = JSONSchemaParser::parse(fromjson("{format: 'email'}"), ignoreUnknownKeywords);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword 'format' is not currently supported");

    result = JSONSchemaParser::parse(fromjson("{id: 'someschema.json'}"), ignoreUnknownKeywords);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword 'id' is not currently supported");

    result = JSONSchemaParser::parse(BSON("$ref"
                                          << "#/definitions/positiveInt"),
                                     ignoreUnknownKeywords);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword '$ref' is not currently supported");

    result = JSONSchemaParser::parse(fromjson("{$schema: 'hyper-schema'}"), ignoreUnknownKeywords);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword '$schema' is not currently supported");

    result = JSONSchemaParser::parse(
        fromjson("{$schema: 'http://json-schema.org/draft-04/schema#'}"), ignoreUnknownKeywords);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword '$schema' is not currently supported");
}

TEST(JSONSchemaParserTest, FailsToParseIfItemsIsNotAnArrayOrObject) {
    auto schema = BSON("items" << 1);
    ASSERT_EQ(JSONSchemaParser::parse(schema).getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseIfItemsIsAnArrayWithANonObject) {
    auto schema = fromjson("{items: [{type: 'string'}, 'blah']}");
    ASSERT_EQ(JSONSchemaParser::parse(schema).getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseIfItemsIsAnInvalidSchema) {
    auto schema = BSON("items" << BSON("invalid" << 1));
    ASSERT_EQ(JSONSchemaParser::parse(schema).getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, FailsToParseIfItemsIsAnArrayThatContainsAnInvalidSchema) {
    auto schema = fromjson("{items: [{type: 'string'}, {invalid: 1}]}");
    ASSERT_EQ(JSONSchemaParser::parse(schema).getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, ItemsParsesSuccessfullyAsArrayAtTopLevel) {
    auto schema = fromjson("{items: [{type: 'string'}]}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{$alwaysTrue: 1}"));
}

TEST(JSONSchemaParserTest, ItemsParsesSuccessfullyAsObjectAtTopLevel) {
    auto schema = fromjson("{items: {type: 'string'}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{$alwaysTrue: 1}"));
}

TEST(JSONSchemaParserTest, ItemsParsesSuccessfullyAsArrayInNestedSchema) {
    auto schema = fromjson("{properties: {a: {items: [{maxLength: 4}, {minimum: 0}]}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    auto expectedResult = fromjson(R"(
        {
            $or: [
                {$nor: [{a: {$exists: true}}]},
                {$nor: [{a: {$_internalSchemaType: [4]}}]},
                {
                    $and: [
                        {
                            a: {
                                $_internalSchemaMatchArrayIndex: {
                                    index: 0,
                                    namePlaceholder: "i",
                                    expression: {
                                        $or: [
                                            {$nor: [{i: {$_internalSchemaType: [2]}}]},
                                            {i: {$_internalSchemaMaxLength: 4}}
                                        ]
                                    }
                                }
                            }
                        },
                        {
                            a: {
                                $_internalSchemaMatchArrayIndex: {
                                    index: 1,
                                    namePlaceholder: "i",
                                    expression: {
                                        $or: [
                                            {
                                                $nor: [
                                                    {i: {$_internalSchemaType: ['number']}}
                                                ]
                                            },
                                            {i: {$gte: 0}}
                                        ]
                                    }
                                }
                            }
                        }
                    ]
                }
            ]
        })");
    ASSERT_SERIALIZES_TO(optimizedResult, expectedResult);
}

TEST(JSONSchemaParserTest, ItemsParsesSuccessfullyAsObjectInNestedSchema) {
    auto schema = fromjson("{properties: {a: {items: {type: 'string'}}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{a: {$exists: true}}]},
                                {$nor: [{a: {$_internalSchemaType: [4]}}]},
                                {
                                    a: {
                                        $_internalSchemaAllElemMatchFromIndex: [
                                            0,
                                            {i: {$_internalSchemaType: [2]}}
                                        ]
                                    }
                                }
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, FailsToParseIfAdditionalItemsIsNotAnObjectOrBoolean) {
    auto schema = BSON("items" << BSONObj() << "additionalItems" << 1);
    ASSERT_EQ(JSONSchemaParser::parse(schema).getStatus(), ErrorCodes::TypeMismatch);

    schema = BSON("additionalItems" << 1);
    ASSERT_EQ(JSONSchemaParser::parse(schema).getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseIfAdditionalItemsIsAnInvalidSchema) {
    auto schema = BSON("items" << BSONObj() << "additionalItems" << BSON("invalid" << 1));
    ASSERT_EQ(JSONSchemaParser::parse(schema).getStatus(), ErrorCodes::FailedToParse);

    schema = BSON("additionalItems" << BSON("invalid" << 1));
    ASSERT_EQ(JSONSchemaParser::parse(schema).getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, AdditionalItemsTranslatesSucessfullyAsBooleanAtTopLevel) {
    auto schema = fromjson("{items: [], additionalItems: true}");
    auto expr = JSONSchemaParser::parse(schema);
    ASSERT_OK(expr.getStatus());
    auto optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    ASSERT_SERIALIZES_TO(optimizedExpr, fromjson("{$and: [{$alwaysTrue: 1}, {$alwaysTrue: 1}]}"));

    schema = fromjson("{items: [], additionalItems: false}");
    expr = JSONSchemaParser::parse(schema);
    ASSERT_OK(expr.getStatus());
    optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    ASSERT_SERIALIZES_TO(optimizedExpr, fromjson("{$and: [{$alwaysTrue: 1}, {$alwaysTrue: 1}]}"));
}

TEST(JSONSchemaParserTest, AdditionalItemsTranslatesSucessfullyAsObjectAtTopLevel) {
    auto schema = fromjson("{items: [], additionalItems: {multipleOf: 7}}");
    auto expr = JSONSchemaParser::parse(schema);
    ASSERT_OK(expr.getStatus());
    auto optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    ASSERT_SERIALIZES_TO(optimizedExpr, fromjson("{$and: [{$alwaysTrue: 1}, {$alwaysTrue: 1}]}"));
}

TEST(JSONSchemaParserTest, AdditionalItemsTranslatesSucessfullyAsBooleanInNestedSchema) {
    auto schema = fromjson("{properties: {a: {items: [], additionalItems: true}}}");
    auto expr = JSONSchemaParser::parse(schema);
    ASSERT_OK(expr.getStatus());
    auto optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    auto expectedResult = fromjson(R"(
        {
            $or: [
                {$nor: [{a: {$exists: true}}]},
                {
                    $and: [
                        {
                            $or: [
                                {$nor: [{a: {$_internalSchemaType: [4]}}]},
                                {}
                            ]
                        },
                        {
                            $or: [
                                {$nor: [{a: {$_internalSchemaType: [4]}}]},
                                {a: {$_internalSchemaAllElemMatchFromIndex: [0, {$alwaysTrue: 1}]}}
                            ]
                        }
                    ]
                }
            ]
        })");
    ASSERT_SERIALIZES_TO(optimizedExpr, expectedResult);

    schema = fromjson("{properties: {a: {items: [], additionalItems: false}}}");
    expr = JSONSchemaParser::parse(schema);
    ASSERT_OK(expr.getStatus());
    optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    expectedResult = fromjson(R"(
        {
            $or: [
                {$nor: [{a: {$exists: true}}]},
                {
                    $and: [
                        {
                            $or: [
                                {$nor: [{a: {$_internalSchemaType: [4]}}]},
                                {}
                            ]
                        },
                        {
                            $or: [
                                {$nor: [{a: {$_internalSchemaType: [4]}}]},
                                {a: {$_internalSchemaAllElemMatchFromIndex: [0, {$alwaysFalse: 1}]}}
                            ]
                        }
                    ]
                }
            ]
        })");
    ASSERT_SERIALIZES_TO(optimizedExpr, expectedResult);
}

TEST(JSONSchemaParserTest, AdditionalItemsGeneratesEmptyExpressionAtTopLevelIfItemsNotPresent) {
    auto schema = BSON("additionalItems" << true);
    auto expr = JSONSchemaParser::parse(schema);
    ASSERT_OK(expr.getStatus());
    auto optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    ASSERT_SERIALIZES_TO(optimizedExpr, BSONObj());

    schema = BSON("additionalItems" << false);
    expr = JSONSchemaParser::parse(schema);
    ASSERT_OK(expr.getStatus());
    optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    ASSERT_SERIALIZES_TO(optimizedExpr, BSONObj());

    schema = BSON("additionalItems" << BSON("minLength" << 1));
    expr = JSONSchemaParser::parse(schema);
    ASSERT_OK(expr.getStatus());
    optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    ASSERT_SERIALIZES_TO(optimizedExpr, BSONObj());
}

TEST(JSONSchemaParserTest, AdditionalItemsGeneratesEmptyExpressionInNestedSchemaIfItemsNotPresent) {
    auto schema = fromjson("{properties: {foo: {additionalItems: true}}}");
    auto expr = JSONSchemaParser::parse(schema);
    ASSERT_OK(expr.getStatus());
    auto optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    ASSERT_SERIALIZES_TO(optimizedExpr, fromjson(R"({
                            $or: [
                                {$nor: [{foo: {$exists: true}}]},
                                {}
                            ]
                        })"));

    schema = fromjson("{properties: {foo: {additionalItems: false}}}");
    expr = JSONSchemaParser::parse(schema);
    ASSERT_OK(expr.getStatus());
    optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    ASSERT_SERIALIZES_TO(optimizedExpr, fromjson(R"({
                            $or: [
                                {$nor: [{foo: {$exists: true}}]},
                                {}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, AdditionalItemsGeneratesEmptyExpressionIfItemsAnObject) {
    auto schema = fromjson("{properties: {a: {items: {minimum: 7}, additionalItems: false}}}");
    auto expr = JSONSchemaParser::parse(schema);
    ASSERT_OK(expr.getStatus());
    auto optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    auto expectedResult = fromjson(R"(
        {
            $or: [
                {$nor: [{a: {$exists: true}}]},
                {$nor: [{a: {$_internalSchemaType: [4]}}]},
                {
                    a: {
                        $_internalSchemaAllElemMatchFromIndex: [
                            0,
                            {
                                $or: [
                                    {$nor: [{i: {$_internalSchemaType: ['number']}}]},
                                    {i: {$gte: 7}}
                                ]
                            }
                        ]
                    }
                }
            ]
        })");
    ASSERT_SERIALIZES_TO(optimizedExpr, expectedResult);

    schema = fromjson("{properties: {a: {items: {minimum: 7}, additionalItems: {minLength: 7}}}}");
    expr = JSONSchemaParser::parse(schema);
    ASSERT_OK(expr.getStatus());
    optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    expectedResult = fromjson(R"(
        {
            $or: [
                {$nor: [{a: {$exists: true}}]},
                {$nor: [{a: {$_internalSchemaType: [4]}}]},
                {
                    a: {
                        $_internalSchemaAllElemMatchFromIndex: [
                            0,
                            {
                                $or: [
                                    {$nor: [{i: {$_internalSchemaType: ['number']}}]},
                                    {i: {$gte: 7}}
                                ]
                            }
                        ]
                    }
                }
            ]
        })");
    ASSERT_SERIALIZES_TO(optimizedExpr, expectedResult);
}

TEST(JSONSchemaParserTest, FailsToParseIfEnumIsNotAnArray) {
    BSONObj schema = fromjson("{properties: {foo: {enum: 'foo'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseEnumIfArrayIsEmpty) {
    BSONObj schema = fromjson("{properties: {foo: {enum: []}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, FailsToParseEnumIfArrayContainsDuplicateValue) {
    BSONObj schema = fromjson("{properties: {foo: {enum: [1, 2, 1]}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);

    schema = fromjson("{properties: {foo: {enum: [{a: 1, b: 1}, {b: 1, a: 1}]}}}");
    result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, EnumTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {foo: {enum: [1, '2', [3]]}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $or: [
                                {$nor: [{foo: {$exists: true}}]},
                                {foo: {$_internalSchemaEq: 1}},
                                {foo: {$_internalSchemaEq: "2"}},
                                {foo: {$_internalSchemaEq: [3]}}
                            ]
                        })"));
}

TEST(JSONSchemaParserTest, TopLevelEnumTranslatesCorrectly) {
    BSONObj schema = fromjson("{enum: [1, {foo: 1}]}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{$_internalSchemaRootDocEq: {foo: 1}}"));
}

TEST(JSONSchemaParserTest, TopLevelEnumWithZeroObjectsTranslatesCorrectly) {
    BSONObj schema = fromjson("{enum: [1, 'impossible', true]}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{$alwaysFalse: 1}"));
}

}  // namespace
}  // namespace mongo
