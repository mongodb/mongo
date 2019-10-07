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

TEST(JSONSchemaArrayKeywordTest, FailsToParseIfMinItemsIsNotANumber) {
    auto schema = BSON("minItems" << BSON_ARRAY(1));
    ASSERT_EQ(JSONSchemaParser::parse(new ExpressionContextForTest(), schema).getStatus(),
              ErrorCodes::FailedToParse);
}

TEST(JSONSchemaArrayKeywordTest, FailsToParseIfMinItemsIsNotANonNegativeInteger) {
    auto schema = BSON("minItems" << -1);
    ASSERT_EQ(JSONSchemaParser::parse(new ExpressionContextForTest(), schema).getStatus(),
              ErrorCodes::FailedToParse);

    schema = BSON("minItems" << 3.14);
    ASSERT_EQ(JSONSchemaParser::parse(new ExpressionContextForTest(), schema).getStatus(),
              ErrorCodes::FailedToParse);
}

TEST(JSONSchemaArrayKeywordTest, MinItemsTranslatesCorrectlyWithNoType) {
    auto schema = BSON("minItems" << 1);
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{}"));

    schema = fromjson("{properties: {a: {minItems: 1}}}");
    result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{a: {$not: {$exists: true}}},
                     {a: {$not: {$_internalSchemaType: [4]}}},
                     {a: {$_internalSchemaMinItems: 1}}]
                })"));
}

TEST(JSONSchemaArrayKeywordTest, MinItemsTranslatesCorrectlyWithArrayType) {
    auto schema = fromjson("{properties: {a: {minItems: 1, type: 'array'}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $or:
            [{a: {$not: {$exists: true}}},
             {$and: [ {a: {$_internalSchemaMinItems: 1}}, {a: {$_internalSchemaType: [4]}}]}]
        })"));
}

TEST(JSONSchemaArrayKeywordTest, MinItemsTranslatesCorrectlyWithNonArrayType) {
    auto schema = fromjson("{properties: {a: {minItems: 1, type: 'number'}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $or:
            [{a: {$not: {$exists: true}}}, {a: {$_internalSchemaType: ["number"]}}]
        })"));
}

TEST(JSONSchemaArrayKeywordTest, FailsToParseIfMaxItemsIsNotANumber) {
    auto schema = BSON("maxItems" << BSON_ARRAY(1));
    ASSERT_EQ(JSONSchemaParser::parse(new ExpressionContextForTest(), schema).getStatus(),
              ErrorCodes::FailedToParse);
}

TEST(JSONSchemaArrayKeywordTest, FailsToParseIfMaxItemsIsNotANonNegativeInteger) {
    auto schema = BSON("maxItems" << -1);
    ASSERT_EQ(JSONSchemaParser::parse(new ExpressionContextForTest(), schema).getStatus(),
              ErrorCodes::FailedToParse);

    schema = BSON("maxItems" << 1.60217);
    ASSERT_EQ(JSONSchemaParser::parse(new ExpressionContextForTest(), schema).getStatus(),
              ErrorCodes::FailedToParse);
}

TEST(JSONSchemaArrayKeywordTest, MaxItemsTranslatesCorrectlyWithNoType) {
    auto schema = BSON("maxItems" << 1);
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{}"));

    schema = fromjson("{properties: {a: {maxItems: 1}}}");
    result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                $or:
                    [{a: {$not: {$exists: true}}},
                     {a: {$not: {$_internalSchemaType: [4]}}},
                     {a: {$_internalSchemaMaxItems: 1}}]
                })"));
}

TEST(JSONSchemaArrayKeywordTest, MaxItemsTranslatesCorrectlyWithArrayType) {
    auto schema = fromjson("{properties: {a: {maxItems: 1, type: 'array'}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $or:
            [{a: {$not: {$exists: true}}},
             {$and: [ {a: {$_internalSchemaMaxItems: 1}}, {a: {$_internalSchemaType: [4]}}]}]
        })"));
}

TEST(JSONSchemaArrayKeywordTest, MaxItemsTranslatesCorrectlyWithNonArrayType) {
    auto schema = fromjson("{properties: {a: {maxItems: 1, type: 'string'}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $or:
            [{a: {$not: {$exists : true}}}, {a: {$_internalSchemaType: [2]}}]
        })"));
}

TEST(JSONSchemaArrayKeywordTest, FailsToParseIfUniqueItemsIsNotABoolean) {
    auto schema = BSON("uniqueItems" << 1);
    ASSERT_EQ(JSONSchemaParser::parse(new ExpressionContextForTest(), schema).getStatus(),
              ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaArrayKeywordTest, UniqueItemsFalseGeneratesAlwaysTrueExpression) {
    auto schema = fromjson("{properties: {a: {uniqueItems: false}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{}"));
}

TEST(JSONSchemaArrayKeywordTest, UniqueItemsTranslatesCorrectlyWithNoType) {
    auto schema = BSON("uniqueItems" << true);
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{}"));

    schema = fromjson("{properties: {a: {uniqueItems: true}}}");
    result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $or:
            [{a: {$not: {$exists: true}}},
             {a: {$not: {$_internalSchemaType: [4]}}},
             {a: {$_internalSchemaUniqueItems: true}}]
        })"));
}

TEST(JSONSchemaArrayKeywordTest, UniqueItemsTranslatesCorrectlyWithTypeArray) {
    auto schema = fromjson("{properties: {a: {type: 'array', uniqueItems: true}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $or:
            [{a: {$not: {$exists: true}}},
             {$and: [ {a: {$_internalSchemaUniqueItems: true}}, {a: {$_internalSchemaType: [4]}}]}]
        })"));
}

TEST(JSONSchemaArrayKeywordTest, FailsToParseIfItemsIsNotAnArrayOrObject) {
    auto schema = BSON("items" << 1);
    ASSERT_EQ(JSONSchemaParser::parse(new ExpressionContextForTest(), schema).getStatus(),
              ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaArrayKeywordTest, FailsToParseIfItemsIsAnArrayWithANonObject) {
    auto schema = fromjson("{items: [{type: 'string'}, 'blah']}");
    ASSERT_EQ(JSONSchemaParser::parse(new ExpressionContextForTest(), schema).getStatus(),
              ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaArrayKeywordTest, FailsToParseIfItemsIsAnInvalidSchema) {
    auto schema = BSON("items" << BSON("invalid" << 1));
    ASSERT_EQ(JSONSchemaParser::parse(new ExpressionContextForTest(), schema).getStatus(),
              ErrorCodes::FailedToParse);
}

TEST(JSONSchemaArrayKeywordTest, FailsToParseIfItemsIsAnArrayThatContainsAnInvalidSchema) {
    auto schema = fromjson("{items: [{type: 'string'}, {invalid: 1}]}");
    ASSERT_EQ(JSONSchemaParser::parse(new ExpressionContextForTest(), schema).getStatus(),
              ErrorCodes::FailedToParse);
}

TEST(JSONSchemaArrayKeywordTest, ItemsParsesSuccessfullyAsArrayAtTopLevel) {
    auto schema = fromjson("{items: [{type: 'string'}]}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{}"));
}

TEST(JSONSchemaArrayKeywordTest, ItemsParsesSuccessfullyAsObjectAtTopLevel) {
    auto schema = fromjson("{items: {type: 'string'}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{}"));
}

TEST(JSONSchemaArrayKeywordTest, ItemsParsesSuccessfullyAsArrayInNestedSchema) {
    auto schema = fromjson("{properties: {a: {items: [{maxLength: 4}, {minimum: 0}]}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    auto expectedResult = fromjson(R"(
        {
        $or:
            [{a: {$not: {$exists: true}}}, {a: {$not: {$_internalSchemaType: [4]}}}, {
                $and: [
                    {
                      a: {
                          $_internalSchemaMatchArrayIndex: {
                              index: 0,
                              namePlaceholder: "i",
                              expression: {
                                  $or: [
                                      {i: {$not: {$_internalSchemaType: [2]}}},
                                      {i: {$_internalSchemaMaxLength: 4}}
                                  ]
                              }
                          }
                      }
                    },
                    {
                      a: {
                          $_internalSchemaMatchArrayIndex : {
                              index: 1,
                              namePlaceholder: "i",
                              expression: {
                                  $or: [
                                      {i : {$not: {$_internalSchemaType: ["number"]}}},
                                      {i: {$gte: 0}}
                                  ]
                              }
                          }
                      }
                    }
                ]
            }]
        })");
    ASSERT_SERIALIZES_TO(optimizedResult, expectedResult);
}

TEST(JSONSchemaArrayKeywordTest, ItemsParsesSuccessfullyAsObjectInNestedSchema) {
    auto schema = fromjson("{properties: {a: {items: {type: 'string'}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $or:
            [{a: {$not: {$exists: true}}},
             {a: {$not: {$_internalSchemaType: [4]}}},
             {a: {$_internalSchemaAllElemMatchFromIndex : [ 0, {i: {$_internalSchemaType: [2]}}]}}]
        })"));
}

TEST(JSONSchemaArrayKeywordTest, FailsToParseIfAdditionalItemsIsNotAnObjectOrBoolean) {
    auto schema = BSON("items" << BSONObj() << "additionalItems" << 1);
    ASSERT_EQ(JSONSchemaParser::parse(new ExpressionContextForTest(), schema).getStatus(),
              ErrorCodes::TypeMismatch);

    schema = BSON("additionalItems" << 1);
    ASSERT_EQ(JSONSchemaParser::parse(new ExpressionContextForTest(), schema).getStatus(),
              ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaArrayKeywordTest, FailsToParseIfAdditionalItemsIsAnInvalidSchema) {
    auto schema = BSON("items" << BSONObj() << "additionalItems" << BSON("invalid" << 1));
    ASSERT_EQ(JSONSchemaParser::parse(new ExpressionContextForTest(), schema).getStatus(),
              ErrorCodes::FailedToParse);

    schema = BSON("additionalItems" << BSON("invalid" << 1));
    ASSERT_EQ(JSONSchemaParser::parse(new ExpressionContextForTest(), schema).getStatus(),
              ErrorCodes::FailedToParse);
}

TEST(JSONSchemaArrayKeywordTest, AdditionalItemsTranslatesSucessfullyAsBooleanAtTopLevel) {
    auto schema = fromjson("{items: [], additionalItems: true}");
    auto expr = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(expr.getStatus());
    auto optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    ASSERT_SERIALIZES_TO(optimizedExpr, fromjson("{}"));

    schema = fromjson("{items: [], additionalItems: false}");
    expr = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(expr.getStatus());
    optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    ASSERT_SERIALIZES_TO(optimizedExpr, fromjson("{}"));
}

TEST(JSONSchemaArrayKeywordTest, AdditionalItemsTranslatesSucessfullyAsObjectAtTopLevel) {
    auto schema = fromjson("{items: [], additionalItems: {multipleOf: 7}}");
    auto expr = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(expr.getStatus());
    auto optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    ASSERT_SERIALIZES_TO(optimizedExpr, fromjson("{}"));
}

TEST(JSONSchemaArrayKeywordTest, AdditionalItemsTranslatesSucessfullyAsBooleanInNestedSchema) {
    auto schema = fromjson("{properties: {a: {items: [], additionalItems: true}}}");
    auto expr = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(expr.getStatus());
    auto optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    auto expectedResult = fromjson(R"(
        {
        $or:
            [{a: {$not: {$exists: true}}},
             {a: {$not: {$_internalSchemaType: [4]}}},
             {a: {$_internalSchemaAllElemMatchFromIndex: [0, {$alwaysTrue: 1}]}}]
        })");
    ASSERT_SERIALIZES_TO(optimizedExpr, expectedResult);

    schema = fromjson("{properties: {a: {items: [], additionalItems: false}}}");
    expr = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(expr.getStatus());
    optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    expectedResult = fromjson(R"(
        {
        $or:
            [{a: {$not: {$exists: true}}},
             {a: {$not: {$_internalSchemaType: [4]}}},
             {a: {$_internalSchemaAllElemMatchFromIndex: [0, {$alwaysFalse: 1}]}}]
        })");
    ASSERT_SERIALIZES_TO(optimizedExpr, expectedResult);
}

TEST(JSONSchemaArrayKeywordTest,
     AdditionalItemsGeneratesEmptyExpressionAtTopLevelIfItemsNotPresent) {
    auto schema = BSON("additionalItems" << true);
    auto expr = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(expr.getStatus());
    auto optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    ASSERT_SERIALIZES_TO(optimizedExpr, fromjson("{}"));

    schema = BSON("additionalItems" << false);
    expr = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(expr.getStatus());
    optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    ASSERT_SERIALIZES_TO(optimizedExpr, fromjson("{}"));

    schema = BSON("additionalItems" << BSON("minLength" << 1));
    expr = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(expr.getStatus());
    optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    ASSERT_SERIALIZES_TO(optimizedExpr, fromjson("{}"));
}

TEST(JSONSchemaArrayKeywordTest,
     AdditionalItemsGeneratesEmptyExpressionInNestedSchemaIfItemsNotPresent) {
    auto schema = fromjson("{properties: {foo: {additionalItems: true}}}");
    auto expr = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(expr.getStatus());
    auto optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    ASSERT_SERIALIZES_TO(optimizedExpr, fromjson("{}"));


    schema = fromjson("{properties: {foo: {additionalItems: false}}}");
    expr = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(expr.getStatus());
    optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    ASSERT_SERIALIZES_TO(optimizedExpr, fromjson("{}"));
}

TEST(JSONSchemaArrayKeywordTest, AdditionalItemsGeneratesEmptyExpressionIfItemsAnObject) {
    auto schema = fromjson("{properties: {a: {items: {minimum: 7}, additionalItems: false}}}");
    auto expr = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(expr.getStatus());
    auto optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    auto expectedResult = fromjson(R"(
        {
        $or:
            [{a: {$not: {$exists: true}}},
             {a: {$not: {$_internalSchemaType: [4]}}},
             {
               a: {
                   $_internalSchemaAllElemMatchFromIndex: [
                       0,
                       {$or: [ {i: {$not: {$_internalSchemaType: ["number"]}}}, {i: {$gte: 7}}]}
                   ]
               }
             }]
        })");
    ASSERT_SERIALIZES_TO(optimizedExpr, expectedResult);

    schema = fromjson("{properties: {a: {items: {minimum: 7}, additionalItems: {minLength: 7}}}}");
    expr = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(expr.getStatus());
    optimizedExpr = MatchExpression::optimize(std::move(expr.getValue()));
    expectedResult = fromjson(R"(
        {
        $or:
            [{a: {$not: {$exists: true}}},
             {a: {$not: {$_internalSchemaType: [4]}}},
             {
               a: {
                   $_internalSchemaAllElemMatchFromIndex : [
                       0,
                       {$or: [ {i: {$not: {$_internalSchemaType: ["number"]}}}, {i: {$gte: 7}}]}
                   ]
               }
             }]
        })");
    ASSERT_SERIALIZES_TO(optimizedExpr, expectedResult);
}

}  // namespace
}  // namespace mongo
