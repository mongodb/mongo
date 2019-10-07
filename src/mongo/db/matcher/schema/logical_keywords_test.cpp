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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{foo: {$not: {$exists: true}}}, {foo: {$not: {$_internalSchemaType: ['number']}}}]
                })"));
}

TEST(JSONSchemaLogicalKeywordTest, TopLevelNotTranslatesCorrectly) {
    BSONObj schema = fromjson("{not: {properties: {foo: {type: 'string'}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
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
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $or:
            [{foo: {$not: {$exists: true}}},
             {foo: {$_internalSchemaEq: 1}},
             {foo: {$_internalSchemaEq: "2"}},
             {foo: {$_internalSchemaEq: [3]}}]
        })"));
}

TEST(JSONSchemaLogicalKeywordTest, TopLevelEnumTranslatesCorrectly) {
    BSONObj schema = fromjson("{enum: [1, {foo: 1}]}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{$_internalSchemaRootDocEq: {foo: 1}}"));
}

TEST(JSONSchemaLogicalKeywordTest, TopLevelEnumWithZeroObjectsTranslatesCorrectly) {
    BSONObj schema = fromjson("{enum: [1, 'impossible', true]}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{$alwaysFalse: 1}"));
}

}  // namespace
}  // namespace mongo
