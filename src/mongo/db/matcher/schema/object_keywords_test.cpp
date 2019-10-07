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

TEST(JSONSchemaObjectKeywordTest, FailsToParseIfTypeIsNotAString) {
    BSONObj schema = fromjson("{type: 1}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseNicelyIfTypeIsKnownUnsupportedAlias) {
    BSONObj schema = fromjson("{type: 'integer'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema type 'integer' is not currently supported");
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseUnknownKeyword) {
    BSONObj schema = fromjson("{unknown: 1}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseIfPropertiesIsNotAnObject) {
    BSONObj schema = fromjson("{properties: 1}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseIfPropertiesIsNotAnObjectWithType) {
    BSONObj schema = fromjson("{type: 'string', properties: 1}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseIfParticularPropertyIsNotAnObject) {
    BSONObj schema = fromjson("{properties: {foo: 1}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseIfKeywordIsDuplicated) {
    BSONObj schema = BSON("type"
                          << "object"
                          << "type"
                          << "object");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaObjectKeywordTest, EmptySchemaTranslatesCorrectly) {
    BSONObj schema = fromjson("{}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{}"));
}

TEST(JSONSchemaObjectKeywordTest, TypeObjectTranslatesCorrectly) {
    BSONObj schema = fromjson("{type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{}"));
}

TEST(JSONSchemaObjectKeywordTest, NestedTypeObjectTranslatesCorrectly) {
    BSONObj schema =
        fromjson("{properties: {a: {type: 'object', properties: {b: {type: 'string'}}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(
        optimizedResult,
        fromjson("{$or: [{a: {$not: {$exists: true }}}, {$and: [{a: {$_internalSchemaObjectMatch: "
                 "{$or: [{b: {$not: {$exists: true}}}, {b: {$_internalSchemaType: [2]}}]}}}, {a: "
                 "{$_internalSchemaType: [3]}}]}]}"));
}

TEST(JSONSchemaObjectKeywordTest, TopLevelNonObjectTypeTranslatesCorrectly) {
    BSONObj schema = fromjson("{type: 'string'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, BSON(AlwaysFalseMatchExpression::kName << 1));
}

TEST(JSONSchemaObjectKeywordTest, TypeNumberTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {num: {type: 'number'}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult,
                         fromjson("{$or: [{num: {$not: {$exists: true }}}, {num: "
                                  "{ $_internalSchemaType: [ 'number' ]}}]}"));
}

TEST(JSONSchemaObjectKeywordTest, RequiredFailsToParseIfNotAnArray) {
    BSONObj schema = fromjson("{required: 'field'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaObjectKeywordTest, RequiredFailsToParseArrayIsEmpty) {
    BSONObj schema = fromjson("{required: []}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaObjectKeywordTest, RequiredFailsToParseIfArrayContainsNonString) {
    BSONObj schema = fromjson("{required: ['foo', 1]}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaObjectKeywordTest, RequiredFailsToParseIfArrayContainsDuplicates) {
    BSONObj schema = fromjson("{required: ['foo', 'bar', 'foo']}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaObjectKeywordTest, TopLevelRequiredTranslatesCorrectly) {
    BSONObj schema = fromjson("{required: ['foo', 'bar']}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult,
                         fromjson("{$and: [{bar: {$exists: true}}, {foo: {$exists: true}}]}"));
}

TEST(JSONSchemaObjectKeywordTest, TopLevelRequiredTranslatesCorrectlyWithProperties) {
    BSONObj schema = fromjson("{required: ['foo'], properties: {foo: {type: 'number'}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $and: [
                                {foo: {$_internalSchemaType: ['number']}},
                                {foo: {$exists: true}}
                            ]
                        })"));
}

TEST(JSONSchemaObjectKeywordTest, RequiredTranslatesCorrectlyWithMultipleElements) {
    BSONObj schema = fromjson("{properties: {x: {required: ['y', 'z']}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $or:
            [{x: {$not: {$exists: true}}}, {x: {$not: {$_internalSchemaType: [3]}}}, {
                x: {
                    $_internalSchemaObjectMatch:
                        {$and: [ {y: {$exists: true}}, {z: {$exists: true}}]}
                }
            }]
        })"));
}

TEST(JSONSchemaObjectKeywordTest, RequiredTranslatesCorrectlyInsideProperties) {
    BSONObj schema = fromjson("{properties: {x: {required: ['y']}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $or:
            [{x: {$not: {$exists: true}}},
             {x: {$not: {$_internalSchemaType: [3]}}},
             {x: {$_internalSchemaObjectMatch: {y: {$exists: true}}}}]
        })"));
}

TEST(JSONSchemaObjectKeywordTest,
     RequiredTranslatesCorrectlyInsidePropertiesWithSiblingProperties) {
    BSONObj schema =
        fromjson("{properties: {x:{required: ['y'], properties: {y: {type: 'number'}}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    auto expectedResult = fromjson(
        R"({
        $or:
            [{x: {$not: {$exists: true}}},
             {
               $and: [
                   {
                     $or: [
                         {x: {$not: {$_internalSchemaType: [3]}}},
                         {x: {$_internalSchemaObjectMatch: {y : {$_internalSchemaType: ["number"]}}}}
                     ]
                   },
                   {
                     $or: [
                         {x: {$not: {$_internalSchemaType: [3]}}},
                         {x: {$_internalSchemaObjectMatch: {y: {$exists: true}}}}
                     ]
                   }
               ]
             }]
        })");
    ASSERT_SERIALIZES_TO(optimizedResult, expectedResult);
}

TEST(JSONSchemaObjectKeywordTest, SharedJsonAndBsonTypeAliasesTranslateIdentically) {
    for (auto&& mapEntry : MatcherTypeSet::kJsonSchemaTypeAliasMap) {
        auto typeAlias = mapEntry.first;
        // JSON Schema spells its bool type as "boolean", whereas MongoDB calls it "bool".
        auto bsonTypeAlias =
            (typeAlias == JSONSchemaParser::kSchemaTypeBoolean) ? "bool" : typeAlias;

        BSONObj typeSchema = BSON("properties" << BSON("f" << BSON("type" << typeAlias)));
        BSONObj bsonTypeSchema =
            BSON("properties" << BSON("f" << BSON("bsonType" << bsonTypeAlias)));
        auto typeResult = JSONSchemaParser::parse(new ExpressionContextForTest(), typeSchema);
        ASSERT_OK(typeResult.getStatus());
        auto bsonTypeResult =
            JSONSchemaParser::parse(new ExpressionContextForTest(), bsonTypeSchema);
        ASSERT_OK(bsonTypeResult.getStatus());

        BSONObjBuilder typeBuilder;
        MatchExpression::optimize(std::move(typeResult.getValue()))->serialize(&typeBuilder);

        BSONObjBuilder bsonTypeBuilder;
        MatchExpression::optimize(std::move(bsonTypeResult.getValue()))
            ->serialize(&bsonTypeBuilder);

        ASSERT_BSONOBJ_EQ(typeBuilder.obj(), bsonTypeBuilder.obj());
    }
}

TEST(JSONSchemaObjectKeywordTest, MinPropertiesFailsToParseIfNotNumber) {
    BSONObj schema = fromjson("{minProperties: null}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, MaxPropertiesFailsToParseIfNotNumber) {
    BSONObj schema = fromjson("{maxProperties: null}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, MinPropertiesFailsToParseIfNegative) {
    BSONObj schema = fromjson("{minProperties: -2}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, MaxPropertiesFailsToParseIfNegative) {
    BSONObj schema = fromjson("{maxProperties: -2}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, MinPropertiesFailsToParseIfNotAnInteger) {
    BSONObj schema = fromjson("{minProperties: 1.1}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, MaxPropertiesFailsToParseIfNotAnInteger) {
    BSONObj schema = fromjson("{maxProperties: 1.1}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, TopLevelMinPropertiesTranslatesCorrectly) {
    BSONObj schema = fromjson("{minProperties: 0}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{$_internalSchemaMinProperties: 0}"));
}

TEST(JSONSchemaObjectKeywordTest, TopLevelMaxPropertiesTranslatesCorrectly) {
    BSONObj schema = fromjson("{maxProperties: 0}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson("{$_internalSchemaMaxProperties: 0}"));
}

TEST(JSONSchemaObjectKeywordTest, NestedMinPropertiesTranslatesCorrectly) {
    BSONObj schema =
        fromjson("{properties: {obj: {type: 'object', minProperties: 2}}, required: ['obj']}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
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

TEST(JSONSchemaObjectKeywordTest, NestedMaxPropertiesTranslatesCorrectly) {
    BSONObj schema =
        fromjson("{properties: {obj: {type: 'object', maxProperties: 2}}, required: ['obj']}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
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

TEST(JSONSchemaObjectKeywordTest, NestedMinPropertiesTranslatesCorrectlyWithoutRequired) {
    BSONObj schema = fromjson("{properties: {obj: {type: 'object', minProperties: 2}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    auto expectedResult = fromjson(R"(
        {
        $or:
            [{obj: {$not: {$exists: true}}}, {
                $and: [
                    {obj: {$_internalSchemaObjectMatch: {$_internalSchemaMinProperties: 2}}},
                    {obj: {$_internalSchemaType: [3]}}
                ]
            }]
        })");
    ASSERT_SERIALIZES_TO(optimizedResult, expectedResult);
}

TEST(JSONSchemaObjectKeywordTest, NestedMaxPropertiesTranslatesCorrectlyWithoutRequired) {
    BSONObj schema = fromjson("{properties: {obj: {type: 'object', maxProperties: 2}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    auto expectedResult = fromjson(R"(
        {
        $or:
            [{obj: {$not: {$exists: true}}}, {
                $and: [
                    {obj: {$_internalSchemaObjectMatch: {$_internalSchemaMaxProperties: 2}}},
                    {obj: {$_internalSchemaType: [3]}}
                ]
            }]
        })");
    ASSERT_SERIALIZES_TO(optimizedResult, expectedResult);
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseIfTypeArrayHasRepeatedAlias) {
    BSONObj schema = fromjson("{properties: {obj: {type: ['object', 'string', 'object']}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseIfBsonTypeArrayHasRepeatedAlias) {
    BSONObj schema = fromjson("{properties: {obj: {bsonType: ['object', 'string', 'object']}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseIfTypeArrayIsEmpty) {
    BSONObj schema = fromjson("{properties: {obj: {type: []}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseIfBsonTypeArrayIsEmpty) {
    BSONObj schema = fromjson("{properties: {obj: {bsonType: []}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseIfTypeArrayContainsNonString) {
    BSONObj schema = fromjson("{properties: {obj: {type: [1]}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseIfBsonTypeArrayContainsNonString) {
    BSONObj schema = fromjson("{properties: {obj: {bsonType: [1]}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseIfTypeArrayContainsUnknownAlias) {
    BSONObj schema = fromjson("{properties: {obj: {type: ['objectId']}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseNicelyIfTypeArrayContainsKnownUnsupportedAlias) {
    BSONObj schema = fromjson("{properties: {obj: {type: ['number', 'integer']}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema type 'integer' is not currently supported");
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseIfBsonTypeArrayContainsUnknownAlias) {
    BSONObj schema = fromjson("{properties: {obj: {bsonType: ['unknown']}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, CanTranslateTopLevelTypeArrayWithoutObject) {
    BSONObj schema = fromjson("{type: ['number', 'string']}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_SERIALIZES_TO(result.getValue(), BSON(AlwaysFalseMatchExpression::kName << 1));
}

TEST(JSONSchemaObjectKeywordTest, CanTranslateTopLevelBsonTypeArrayWithoutObject) {
    BSONObj schema = fromjson("{bsonType: ['number', 'string']}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_SERIALIZES_TO(result.getValue(), BSON(AlwaysFalseMatchExpression::kName << 1));
}

TEST(JSONSchemaObjectKeywordTest, CanTranslateTopLevelTypeArrayWithObject) {
    BSONObj schema = fromjson("{type: ['number', 'object']}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_SERIALIZES_TO(result.getValue(), fromjson("{}"));
}

TEST(JSONSchemaObjectKeywordTest, CanTranslateTopLevelBsonTypeArrayWithObject) {
    BSONObj schema = fromjson("{bsonType: ['number', 'object']}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_SERIALIZES_TO(result.getValue(), fromjson("{}"));
}

TEST(JSONSchemaObjectKeywordTest, CanTranslateNestedTypeArray) {
    BSONObj schema = fromjson("{properties: {a: {type: ['number', 'object']}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $or:
            [{a: {$not: {$exists: true}}}, {a: {$_internalSchemaType: [ "number", 3 ]}}]
        })"));
}

TEST(JSONSchemaObjectKeywordTest, CanTranslateNestedBsonTypeArray) {
    BSONObj schema = fromjson("{properties: {a: {bsonType: ['number', 'objectId']}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $or:
            [{a: {$not: {$exists: true}}}, {a: {$_internalSchemaType: [ "number", 7 ]}}]
        })"));
}

TEST(JSONSchemaObjectKeywordTest, DependenciesFailsToParseIfNotAnObject) {
    BSONObj schema = fromjson("{dependencies: []}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, DependenciesFailsToParseIfDependencyIsNotObjectOrArray) {
    BSONObj schema = fromjson("{dependencies: {a: ['b'], bad: 1}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, DependenciesFailsToParseIfNestedSchemaIsInvalid) {
    BSONObj schema = fromjson("{dependencies: {a: {invalid: 1}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, PropertyDependencyFailsToParseIfEmptyArray) {
    BSONObj schema = fromjson("{dependencies: {a: []}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, PropertyDependencyFailsToParseIfArrayContainsNonStringElement) {
    BSONObj schema = fromjson("{dependencies: {a: ['b', 1]}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, PropertyDependencyFailsToParseIfRepeatedArrayElement) {
    BSONObj schema = fromjson("{dependencies: {a: ['b', 'b']}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, TopLevelSchemaDependencyTranslatesCorrectly) {
    BSONObj schema = fromjson("{dependencies: {a: {properties: {b: {type: 'string'}}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $_internalSchemaCond:
            [{a: {$exists: true}},
             {$or: [ {b: {$not: {$exists: true}}}, {b: {$_internalSchemaType: [2]}}]},
             {$alwaysTrue: 1}]
        })"));
}

TEST(JSONSchemaObjectKeywordTest, TopLevelPropertyDependencyTranslatesCorrectly) {
    BSONObj schema = fromjson("{dependencies: {a: ['b', 'c']}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
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

TEST(JSONSchemaObjectKeywordTest, NestedSchemaDependencyTranslatesCorrectly) {
    BSONObj schema =
        fromjson("{properties: {a: {dependencies: {b: {properties: {c: {type: 'object'}}}}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $or:
            [{a: {$not: {$exists: true}}}, {
                $_internalSchemaCond: [
                    {a: {$_internalSchemaObjectMatch : {b: {$exists: true}}}},
                    {
                      $or : [
                          {a: {$not: {$_internalSchemaType: [3]}}},
                          {
                            a: {
                                $_internalSchemaObjectMatch : {
                                    $or : [
                                        {c: {$not: {$exists: true}}},
                                        {c: {$_internalSchemaType: [3]}}
                                    ]
                                }
                            }
                          }
                      ]
                    },
                    {$alwaysTrue : 1}
                ]
            }]
        })"));
}

TEST(JSONSchemaObjectKeywordTest, NestedPropertyDependencyTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {a: {dependencies: {b: ['c', 'd']}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    auto expectedResult = fromjson(R"(
        {
        $or:
            [{a: {$not: {$exists: true}}}, {
                $_internalSchemaCond: [
                    {a: {$_internalSchemaObjectMatch : {b : {$exists : true}}}},
                    {
                      $and: [
                          {a: {$_internalSchemaObjectMatch: {c: {$exists: true}}}},
                          {a: {$_internalSchemaObjectMatch: {d: {$exists: true}}}}
                      ]
                    },
                    {$alwaysTrue: 1}
                ]
            }]
        })");
    ASSERT_SERIALIZES_TO(optimizedResult, expectedResult);
}

TEST(JSONSchemaObjectKeywordTest, EmptyDependenciesTranslatesCorrectly) {
    BSONObj schema = fromjson("{dependencies: {}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_SERIALIZES_TO(result.getValue(), fromjson("{$and: [{}]}"));
}

TEST(JSONSchemaObjectKeywordTest, UnsupportedKeywordsFailNicely) {
    auto result =
        JSONSchemaParser::parse(new ExpressionContextForTest(), fromjson("{default: {}}"));
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword 'default' is not currently supported");

    result = JSONSchemaParser::parse(new ExpressionContextForTest(),
                                     fromjson("{definitions: {numberField: {type: 'number'}}}"));
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword 'definitions' is not currently supported");

    result = JSONSchemaParser::parse(new ExpressionContextForTest(), fromjson("{format: 'email'}"));
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword 'format' is not currently supported");

    result = JSONSchemaParser::parse(new ExpressionContextForTest(),
                                     fromjson("{id: 'someschema.json'}"));
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword 'id' is not currently supported");

    result = JSONSchemaParser::parse(new ExpressionContextForTest(),
                                     BSON("$ref"
                                          << "#/definitions/positiveInt"));
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword '$ref' is not currently supported");

    result = JSONSchemaParser::parse(new ExpressionContextForTest(),
                                     fromjson("{$schema: 'hyper-schema'}"));
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword '$schema' is not currently supported");

    result =
        JSONSchemaParser::parse(new ExpressionContextForTest(),
                                fromjson("{$schema: 'http://json-schema.org/draft-04/schema#'}"));
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword '$schema' is not currently supported");
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseIfDescriptionIsNotAString) {
    auto result =
        JSONSchemaParser::parse(new ExpressionContextForTest(), fromjson("{description: {}}"));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, CorrectlyParsesDescriptionAsString) {
    auto result =
        JSONSchemaParser::parse(new ExpressionContextForTest(), fromjson("{description: 'str'}"));
    ASSERT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, CorrectlyParsesNestedDescriptionAsString) {
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(),
                                          fromjson("{properties: {a: {description: 'str'}}}"));
    ASSERT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseIfTitleIsNotAString) {
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), fromjson("{title: {}}"));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, CorrectlyParsesTitleAsString) {
    auto result =
        JSONSchemaParser::parse(new ExpressionContextForTest(), fromjson("{title: 'str'}"));
    ASSERT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, CorrectlyParsesNestedTitleAsString) {
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(),
                                          fromjson("{properties: {a: {title: 'str'}}}"));
    ASSERT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, PatternPropertiesFailsToParseIfNotObject) {
    BSONObj schema = fromjson("{patternProperties: 1}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, PatternPropertiesFailsToParseIfOnePropertyIsNotObject) {
    BSONObj schema = fromjson("{patternProperties: {a: {}, b: 1}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, PatternPropertiesFailsToParseIfNestedSchemaIsInvalid) {
    BSONObj schema = fromjson("{patternProperties: {a: {invalid: 1}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, PatternPropertiesFailsToParseIfPropertyNameIsAnInvalidRegex) {
    BSONObj schema = fromjson("{patternProperties: {'[': {}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, AdditionalPropertiesFailsToParseIfNotBoolOrString) {
    BSONObj schema = fromjson("{additionalProperties: 1}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, AdditionalPropertiesFailsToParseIfNestedSchemaIsInvalid) {
    BSONObj schema = fromjson("{additionalProperties: {invalid: 1}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaObjectKeywordTest, TopLevelPatternPropertiesTranslatesCorrectly) {
    BSONObj schema =
        fromjson("{patternProperties: {'^a': {type: 'number'}, '^b': {type: 'string'}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
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

TEST(JSONSchemaObjectKeywordTest, TopLevelAdditionalPropertiesFalseTranslatesCorrectly) {
    BSONObj schema = fromjson("{additionalProperties: false}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
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

TEST(JSONSchemaObjectKeywordTest, TopLevelAdditionalPropertiesTrueTranslatesCorrectly) {
    BSONObj schema = fromjson("{additionalProperties: true}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
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

TEST(JSONSchemaObjectKeywordTest, TopLevelAdditionalPropertiesTypeNumberTranslatesCorrectly) {
    BSONObj schema = fromjson("{additionalProperties: {type: 'number'}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
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

TEST(JSONSchemaObjectKeywordTest, NestedAdditionalPropertiesTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {obj: {additionalProperties: {type: 'number'}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
        $or:
            [{obj: {$not: {$exists: true}}}, {obj: {$not: {$_internalSchemaType: [3]}}}, {
                obj: {
                    $_internalSchemaObjectMatch: {
                        $_internalSchemaAllowedProperties: {
                            properties: [],
                            namePlaceholder: "i",
                            patternProperties: [],
                            otherwise: {i: {$_internalSchemaType: ["number"]}}
                        }
                    }
                }
            }]
        })"));
}

TEST(JSONSchemaObjectKeywordTest,
     PropertiesPatternPropertiesAndAdditionalPropertiesTranslateCorrectlyTogether) {
    BSONObj schema = fromjson(
        "{properties: {a: {}, b: {}}, patternProperties: {'^c': {}}, additionalProperties: false}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"({
                            $_internalSchemaAllowedProperties: {
                                properties: ["a", "b"],
                                namePlaceholder: "i",
                                patternProperties: [
                                    {regex: /^c/, expression: {}}
                                ],
                                otherwise: {$alwaysFalse: 1}
                            }
                        })"));
}

TEST(JSONSchemaObjectKeywordTest,
     PropertiesPatternPropertiesAdditionalPropertiesAndRequiredTranslateCorrectlyTogether) {
    BSONObj schema = fromjson(
        "{properties: {a: {}, b: {}}, required: ['a'], patternProperties: {'^c': {}}, "
        "additionalProperties: false}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
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
                                {a: {$exists: true}}
                            ]
                        })"));
}

TEST(JSONSchemaObjectKeywordTest, CorrectlyIgnoresUnknownKeywordsParameterIsSet) {
    const auto ignoreUnknownKeywords = true;

    auto schema = fromjson("{ignored_keyword: 1}");
    ASSERT_OK(JSONSchemaParser::parse(new ExpressionContextForTest(), schema, ignoreUnknownKeywords)
                  .getStatus());

    schema = fromjson("{properties: {a: {ignored_keyword: 1}}}");
    ASSERT_OK(JSONSchemaParser::parse(new ExpressionContextForTest(), schema, ignoreUnknownKeywords)
                  .getStatus());

    schema = fromjson("{properties: {a: {oneOf: [{ignored_keyword: {}}]}}}");
    ASSERT_OK(JSONSchemaParser::parse(new ExpressionContextForTest(), schema, ignoreUnknownKeywords)
                  .getStatus());
}

TEST(JSONSchemaObjectKeywordTest, FailsToParseUnsupportedKeywordsWhenIgnoreUnknownParameterIsSet) {
    const auto ignoreUnknownKeywords = true;

    auto result = JSONSchemaParser::parse(
        new ExpressionContextForTest(), fromjson("{default: {}}"), ignoreUnknownKeywords);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword 'default' is not currently supported");

    result = JSONSchemaParser::parse(new ExpressionContextForTest(),
                                     fromjson("{definitions: {numberField: {type: 'number'}}}"),
                                     ignoreUnknownKeywords);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword 'definitions' is not currently supported");

    result = JSONSchemaParser::parse(
        new ExpressionContextForTest(), fromjson("{format: 'email'}"), ignoreUnknownKeywords);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword 'format' is not currently supported");

    result = JSONSchemaParser::parse(
        new ExpressionContextForTest(), fromjson("{id: 'someschema.json'}"), ignoreUnknownKeywords);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword 'id' is not currently supported");

    result = JSONSchemaParser::parse(new ExpressionContextForTest(),
                                     BSON("$ref"
                                          << "#/definitions/positiveInt"),
                                     ignoreUnknownKeywords);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword '$ref' is not currently supported");

    result = JSONSchemaParser::parse(new ExpressionContextForTest(),
                                     fromjson("{$schema: 'hyper-schema'}"),
                                     ignoreUnknownKeywords);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword '$schema' is not currently supported");

    result =
        JSONSchemaParser::parse(new ExpressionContextForTest(),
                                fromjson("{$schema: 'http://json-schema.org/draft-04/schema#'}"),
                                ignoreUnknownKeywords);
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "$jsonSchema keyword '$schema' is not currently supported");
}

}  // namespace
}  // namespace mongo
