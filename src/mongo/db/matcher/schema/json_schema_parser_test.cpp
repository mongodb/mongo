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
#include "mongo/db/matcher/schema/json_schema_parser.h"
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
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson("{}"));
}

TEST(JSONSchemaParserTest, TypeObjectTranslatesCorrectly) {
    BSONObj schema = fromjson("{type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson("{}"));
}

TEST(JSONSchemaParserTest, NestedTypeObjectTranslatesCorrectly) {
    BSONObj schema =
        fromjson("{properties: {a: {type: 'object', properties: {b: {type: 'string'}}}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                     $and: [{
                         $and: [{
                             $or: [
                                 {$nor: [{a: {$exists: true}}]},
                                 {
                                   $and: [
                                       {
                                           a: {
                                               $_internalSchemaObjectMatch: {
                                                  $and: [{
                                                      $or: [
                                                          {$nor: [{b: {$exists: true}}]},
                                                          {$and: [{b: {$_internalSchemaType: 2}}]}
                                                      ]
                                                  }]
                                               }
                                           }
                                       },
                                       {a: {$_internalSchemaType: 3}}
                                   ]
                                 }
                             ]
                         }]
                     }]
                 })"));
}

TEST(JSONSchemaParserTest, TopLevelNonObjectTypeTranslatesCorrectly) {
    BSONObj schema = fromjson("{type: 'string'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson("{$alwaysFalse: 1}"));
}

TEST(JSONSchemaParserTest, TypeNumberTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {num: {type: 'number'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                                   $and: [{
                                       $and: [{
                                           $or: [
                                               {$nor: [{num: {$exists: true}}]},
                                               {$and: [{num: {$_internalSchemaType: 'number'}}]}
                                           ]
                                       }]
                                   }]
                               })"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithTypeNumber) {
    BSONObj schema = fromjson("{properties: {num: {type: 'number', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                                   $and: [{
                                       $and: [{
                                           $or: [
                                               {$nor: [{num: {$exists: true}}]},
                                               {
                                                 $and: [
                                                     {num: {$lte: 0}},
                                                     {num: {$_internalSchemaType: 'number'}}
                                                 ]
                                               }
                                           ]
                                       }]
                                   }]
                               })"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithTypeLong) {
    BSONObj schema = fromjson("{properties: {num: {type: 'long', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                                   $and: [{
                                       $and: [{
                                           $or: [
                                               {$nor: [{num: {$exists: true}}]},
                                               {
                                                 $and: [
                                                     {num: {$lte: 0}},
                                                     {num: {$_internalSchemaType: 18}}
                                                 ]
                                               }
                                           ]
                                       }]
                                   }]
                               })"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithTypeString) {
    BSONObj schema = fromjson("{properties: {num: {type: 'string', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                     $and: [{
                         $and: [{
                             $or: [
                                 {$nor: [{num: {$exists: true}}]},
                                 {$and: [{$alwaysTrue: 1}, {num: {$_internalSchemaType: 2}}]}
                             ]
                         }]
                     }]
                 })"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithNoType) {
    BSONObj schema = fromjson("{properties: {num: {maximum: 0}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                     $and: [{
                         $and: [{
                             $or: [
                                 {$nor: [{num: {$exists: true}}]},
                                 {
                                   $and: [{
                                       $or: [
                                           {$nor: [{num: {$_internalSchemaType: 'number'}}]},
                                           {num: {$lte: 0}}
                                       ]
                                   }]
                                 }
                             ]
                         }]
                     }]
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
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
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
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                                   $and: [{
                                       $and: [{
                                           $or: [
                                               {$nor: [{num: {$exists: true}}]},
                                               {
                                                 $and: [
                                                     {num: {$gte: 0}},
                                                     {num: {$_internalSchemaType: 'number'}}
                                                 ]
                                               }
                                           ]
                                       }]
                                   }]
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
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                                   $and: [{
                                       $and: [{
                                           $or: [
                                               {$nor: [{foo: {$exists: true}}]},
                                               {
                                                 $and: [
                                                     {foo: {$_internalSchemaMaxLength: 5}},
                                                     {foo: {$_internalSchemaType: 2}}
                                                 ]
                                               }
                                           ]
                                       }]
                                   }]
                               })"));
}

TEST(JSONSchemaParserTest, MaxLengthTranslatesCorrectlyWithTypeString) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', maxLength: 5}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                                   $and: [{
                                       $and: [{
                                           $or: [
                                               {$nor: [{foo: {$exists: true}}]},
                                               {
                                                 $and: [
                                                     {foo: {$_internalSchemaMaxLength: 5}},
                                                     {foo: {$_internalSchemaType: 2}}
                                                 ]
                                               }
                                           ]
                                       }]
                                   }]
                               })"));
}

TEST(JSONSchemaParserTest, MinimumTranslatesCorrectlyWithTypeLong) {
    BSONObj schema = fromjson("{properties: {num: {type: 'long', minimum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                                   $and: [{
                                       $and: [{
                                           $or: [
                                               {$nor: [{num: {$exists: true}}]},
                                               {
                                                 $and: [
                                                     {num: {$gte: 0}},
                                                     {num: {$_internalSchemaType: 18}}
                                                 ]
                                               }
                                           ]
                                       }]
                                   }]
                               })"));
}

TEST(JSONSchemaParserTest, MinimumTranslatesCorrectlyWithTypeString) {
    BSONObj schema = fromjson("{properties: {num: {type: 'string', minimum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                     $and: [{
                         $and: [{
                             $or: [
                                 {$nor: [{num: {$exists: true}}]},
                                 {$and: [{$alwaysTrue: 1}, {num: {$_internalSchemaType: 2}}]}
                             ]
                         }]
                     }]
                 })"));
}

TEST(JSONSchemaParserTest, MinimumTranslatesCorrectlyWithNoType) {
    BSONObj schema = fromjson("{properties: {num: {minimum: 0}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                     $and: [{
                         $and: [{
                             $or: [
                                 {$nor: [{num: {$exists: true}}]},
                                 {
                                   $and: [{
                                       $or: [
                                           {$nor: [{num: {$_internalSchemaType: 'number'}}]},
                                           {num: {$gte: 0}}
                                       ]
                                   }]
                                 }
                             ]
                         }]
                     }]
                 })"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithExclusiveMaximumTrue) {
    BSONObj schema = fromjson(
        "{properties: {num: {type: 'long', maximum: 0, exclusiveMaximum: true}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                                   $and: [{
                                       $and: [{
                                           $or: [
                                               {$nor: [{num: {$exists: true}}]},
                                               {
                                                 $and: [
                                                     {num: {$lt: 0}},
                                                     {num: {$_internalSchemaType: 18}}
                                                 ]
                                               }
                                           ]
                                       }]
                                   }]
                               })"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithExclusiveMaximumFalse) {
    BSONObj schema = fromjson(
        "{properties: {num: {type: 'long', maximum: 0, exclusiveMaximum: false}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                                   $and: [{
                                       $and: [{
                                           $or: [
                                               {$nor: [{num: {$exists: true}}]},
                                               {
                                                 $and: [
                                                     {num: {$lte: 0}},
                                                     {num: {$_internalSchemaType: 18}}
                                                 ]
                                               }
                                           ]
                                       }]
                                   }]
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
        "{properties: {num: {type: 'long', minimum: 0, exclusiveMinimum: true}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                                   $and: [{
                                       $and: [{
                                           $or: [
                                               {$nor: [{num: {$exists: true}}]},
                                               {
                                                 $and: [
                                                     {num: {$gt: 0}},
                                                     {num: {$_internalSchemaType: 18}}
                                                 ]
                                               }
                                           ]
                                       }]
                                   }]
                               })"));
}

TEST(JSONSchemaParserTest, MinimumTranslatesCorrectlyWithExclusiveMinimumFalse) {
    BSONObj schema = fromjson(
        "{properties: {num: {type: 'long', minimum: 0, exclusiveMinimum: false}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                                   $and: [{
                                       $and: [{
                                           $or: [
                                               {$nor: [{num: {$exists: true}}]},
                                               {
                                                 $and: [
                                                     {num: {$gte: 0}},
                                                     {num: {$_internalSchemaType: 18}}
                                                 ]
                                               }
                                           ]
                                       }]
                                   }]
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
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
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
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                                   $and: [{
                                       $and: [{
                                           $or: [
                                               {$nor: [{foo: {$exists: true}}]},
                                               {
                                                 $and: [
                                                     {foo: {$_internalSchemaMinLength: 5}},
                                                     {foo: {$_internalSchemaType: 2}}
                                                 ]
                                               }
                                           ]
                                       }]
                                   }]
                               })"));
}

TEST(JSONSchemaParserTest, MinLengthTranslatesCorrectlyWithIntegralDouble) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', minLength: 5.0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                                   $and: [{
                                       $and: [{
                                           $or: [
                                               {$nor: [{foo: {$exists: true}}]},
                                               {
                                                 $and: [
                                                     {foo: {$_internalSchemaMinLength: 5}},
                                                     {foo: {$_internalSchemaType: 2}}
                                                 ]
                                               }
                                           ]
                                       }]
                                   }]
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
    BSONObj expected =
        BSON("$and" << BSON_ARRAY(BSON(
                 "$and" << BSON_ARRAY(BSON(
                     "$or" << BSON_ARRAY(
                         BSON("$nor" << BSON_ARRAY(BSON("foo" << BSON("$exists" << true))))
                         << BSON("$and" << BSON_ARRAY(
                                     BSON("foo" << BSON("$regex"
                                                        << "abc"))
                                     << BSON("foo" << BSON("$_internalSchemaType" << 2))))))))));
    ASSERT_SERIALIZES_TO(result.getValue().get(), expected);
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
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                     $and: [{
                         $and: [{
                             $or: [
                                 {$nor: [{foo: {$exists: true}}]},
                                 {
                                   $and: [
                                       {foo: {$_internalSchemaFmod: [NumberDecimal('5.3'), 0]}},
                                       {foo: {$_internalSchemaType: 'number'}}
                                   ]
                                 }
                             ]
                         }]
                     }]
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
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
        $and: [{
           $and: [{
               $or: [
                   {$nor: [{foo: {$exists: true}}]},
                   {$and: [{
                        $and: [
                            {$and: [{
                                $or: [ 
                                    {$nor: [{foo: {$_internalSchemaType: "number"}}]}, 
                                    {foo: {$gte: 0}} 
                                ]
                            }]}, 
                            {$and: [{
                                $or: [
                                    {$nor: [{foo: {$_internalSchemaType: "number"}}]},
                                    {foo: {$lte: 10}}
                                ]
                            }]}
                        ]
                    }]}
                ]
            }]
        }]})"));
}

TEST(JSONSchemaParserTest, TopLevelAllOfTranslatesCorrectly) {
    BSONObj schema = fromjson("{allOf: [{properties: {foo: {type: 'string'}}}]}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {$and: [{ 
            $and: [{ 
                $and: [{ 
                    $and: [{
                        $or: [
                            {$nor: [{foo: {$exists: true}}]}, 
                            {$and: [{foo: {$_internalSchemaType: 2}}]} 
                        ]
                    }]
                }]
            }]
        }]})"));
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
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {$and: [{
            $and: [{
                $or: [
                    {$nor: [{foo: {$exists: true}}]}, 
                    {$and: [{
                        $or: [
                            {$and: [{foo: {$_internalSchemaType: "number"}}]}, 
                            {$and: [{foo: {$_internalSchemaType: 2}}]}
                        ]
                    }]}
                ]
            }]
        }]})"));
}

TEST(JSONSchemaParserTest, TopLevelAnyOfTranslatesCorrectly) {
    BSONObj schema = fromjson("{anyOf: [{properties: {foo: {type: 'string'}}}]}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {$and: [{
            $or: [{
                $and: [{
                    $and: [{
                        $or: [
                            {$nor: [{foo: {$exists: true}}]}, 
                            {$and: [{foo: {$_internalSchemaType: 2}}]}
                        ]
                    }]
                }]
            }]
        }]})"));
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
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {$and: [{
            $and: [{
                $or: [
                    {$nor: [{foo: {$exists: true}}]}, 
                    {$and: [{
                        $_internalSchemaXor: [
                            {$and: [{
                                $or: [
                                    {$nor: [{foo: {$_internalSchemaType: "number"}}]}, 
                                    {foo: {$gte: 0}}
                                ]
                            }]}, 
                            {$and: [{
                                $or: [
                                    {$nor: [{foo: {$_internalSchemaType: "number"}}]},
                                    {foo: {$lte: 10}}
                                ]
                            }]}
                        ]
                    }]}
                ]
            }]
        }]})"));
}

TEST(JSONSchemaParserTest, TopLevelOneOfTranslatesCorrectly) {
    BSONObj schema = fromjson("{oneOf: [{properties: {foo: {type: 'string'}}}]}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {$and: [{
            $_internalSchemaXor: [{
                $and: [{
                    $and: [{
                        $or: [
                            {$nor: [{foo: {$exists: true}}]}, 
                            {$and: [{foo: {$_internalSchemaType: 2}}]}
                        ]
                    }]
                }]
            }]
        }]})"));
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
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {$and: [{
            $and: [{
                $or: [
                    {$nor: [{foo: {$exists: true}}]}, 
                    {$and: [{
                        $nor: [{
                            $and: [{foo: {$_internalSchemaType: "number"}}]
                        }]
                    }]}
                ]
            }]
        }]})"));
}

TEST(JSONSchemaParserTest, TopLevelNotTranslatesCorrectly) {
    BSONObj schema = fromjson("{not: {properties: {foo: {type: 'string'}}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {$and: [{
            $nor: [{
                $and: [{
                    $and: [{
                        $or: [
                            {$nor: [{foo: {$exists: true}}]}, 
                            {$and: [{foo: {$_internalSchemaType: 2}}]}
                        ]
                    }]
                }]
            }]
        }]})"));
}

}  // namespace
}  // namespace mongo
