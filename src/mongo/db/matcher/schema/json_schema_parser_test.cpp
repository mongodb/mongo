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
                                                          {$and: [{b: {$_internalSchemaType: [2]}}]}
                                                      ]
                                                  }]
                                               }
                                           }
                                       },
                                       {a: {$_internalSchemaType: [3]}}
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
                                               {$and: [{num: {$_internalSchemaType: ['number']}}]}
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
                                                     {num: {$_internalSchemaType: ['number']}}
                                                 ]
                                               }
                                           ]
                                       }]
                                   }]
                               })"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithBsonTypeLong) {
    BSONObj schema =
        fromjson("{properties: {num: {bsonType: 'long', maximum: 0}}, type: 'object'}");
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
                                                     {num: {$_internalSchemaType: [18]}}
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
                                 {$and: [{$alwaysTrue: 1}, {num: {$_internalSchemaType: [2]}}]}
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
                                           {$nor: [{num: {$_internalSchemaType: ['number']}}]},
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
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                                   $and: [{
                                       $and: [{
                                           $or: [
                                               {$nor: [{num: {$exists: true}}]},
                                               {
                                                 $and: [
                                                     {num: {$gte: 0}},
                                                     {num: {$_internalSchemaType: ['number']}}
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
                                                     {foo: {$_internalSchemaType: [2]}}
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
                                                     {foo: {$_internalSchemaType: [2]}}
                                                 ]
                                               }
                                           ]
                                       }]
                                   }]
                               })"));
}

TEST(JSONSchemaParserTest, MinimumTranslatesCorrectlyWithBsonTypeLong) {
    BSONObj schema =
        fromjson("{properties: {num: {bsonType: 'long', minimum: 0}}, type: 'object'}");
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
                                                     {num: {$_internalSchemaType: [18]}}
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
                                 {$and: [{$alwaysTrue: 1}, {num: {$_internalSchemaType: [2]}}]}
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
                                           {$nor: [{num: {$_internalSchemaType: ['number']}}]},
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
        "{properties: {num: {bsonType: 'long', maximum: 0, exclusiveMaximum: true}},"
        "type: 'object'}");
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
                                                     {num: {$_internalSchemaType: [18]}}
                                                 ]
                                               }
                                           ]
                                       }]
                                   }]
                               })"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithExclusiveMaximumFalse) {
    BSONObj schema = fromjson(
        "{properties: {num: {bsonType: 'long', maximum: 0, exclusiveMaximum: false}},"
        "type: 'object'}");
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
                                                     {num: {$_internalSchemaType: [18]}}
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
        "{properties: {num: {bsonType: 'long', minimum: 0, exclusiveMinimum: true}},"
        "type: 'object'}");
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
                                                     {num: {$_internalSchemaType: [18]}}
                                                 ]
                                               }
                                           ]
                                       }]
                                   }]
                               })"));
}

TEST(JSONSchemaParserTest, MinimumTranslatesCorrectlyWithExclusiveMinimumFalse) {
    BSONObj schema = fromjson(
        "{properties: {num: {bsonType: 'long', minimum: 0, exclusiveMinimum: false}},"
        "type: 'object'}");
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
                                                     {num: {$_internalSchemaType: [18]}}
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
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"({
                                   $and: [{
                                       $and: [{
                                           $or: [
                                               {$nor: [{foo: {$exists: true}}]},
                                               {
                                                 $and: [
                                                     {foo: {$_internalSchemaMinLength: 5}},
                                                     {foo: {$_internalSchemaType: [2]}}
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
                                                     {foo: {$_internalSchemaType: [2]}}
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
    BSONObj expected = BSON(
        "$and" << BSON_ARRAY(BSON(
            "$and" << BSON_ARRAY(BSON(
                "$or" << BSON_ARRAY(
                    BSON("$nor" << BSON_ARRAY(BSON("foo" << BSON("$exists" << true))))
                    << BSON("$and" << BSON_ARRAY(BSON("foo" << BSON("$regex"
                                                                    << "abc"))
                                                 << BSON("foo" << BSON("$_internalSchemaType"
                                                                       << BSON_ARRAY(2)))))))))));
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
                                       {foo: {$_internalSchemaType: ['number']}}
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
                                    {$nor: [{foo: {$_internalSchemaType: ['number']}}]},
                                    {foo: {$gte: 0}}
                                ]
                            }]},
                            {$and: [{
                                $or: [
                                    {$nor: [{foo: {$_internalSchemaType: ['number']}}]},
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
                            {$and: [{foo: {$_internalSchemaType: [2]}}]}
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
                            {$and: [{foo: {$_internalSchemaType: ['number']}}]},
                            {$and: [{foo: {$_internalSchemaType: [2]}}]}
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
                            {$and: [{foo: {$_internalSchemaType: [2]}}]}
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
                                    {$nor: [{foo: {$_internalSchemaType: ['number']}}]},
                                    {foo: {$gte: 0}}
                                ]
                            }]},
                            {$and: [{
                                $or: [
                                    {$nor: [{foo: {$_internalSchemaType: ['number']}}]},
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
                            {$and: [{foo: {$_internalSchemaType: [2]}}]}
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
                            $and: [{foo: {$_internalSchemaType: ['number']}}]
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
                            {$and: [{foo: {$_internalSchemaType: [2]}}]}
                        ]
                    }]
                }]
            }]
        }]})"));
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
    ASSERT_SERIALIZES_TO(result.getValue(), fromjson("{$and: [{$alwaysTrue: 1}]}"));

    schema = fromjson("{properties: {a: {minItems: 1}}}");
    result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());

    ASSERT_SERIALIZES_TO(result.getValue(), fromjson(R"(
        {$and: [{
              $and: [{
                  $or: [
                      {$nor: [{a: {$exists: true}}]},
                      {
                        $and: [{
                            $or: [
                                {$nor: [{a: {$_internalSchemaType: [4]}}]},
                                {a: {$_internalSchemaMinItems: 1}}
                            ]
                        }]
                      }
                  ]
              }]
          }]})"));
}

TEST(JSONSchemaParserTest, MinItemsTranslatesCorrectlyWithArrayType) {
    auto schema = fromjson("{properties: {a: {minItems: 1, type: 'array'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue(), fromjson(R"(
        {$and: [{
              $and: [{
                  $or: [
                      {$nor: [{a: {$exists: true}}]},
                      {$and: [{a: {$_internalSchemaMinItems: 1}}, {a: {$_internalSchemaType: [4]}}]}
                  ]
              }]
        }]})"));
}

TEST(JSONSchemaParserTest, MinItemsTranslatesCorrectlyWithNonArrayType) {
    auto schema = fromjson("{properties: {a: {minItems: 1, type: 'number'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue(), fromjson(R"(
        {$and: [{
              $and: [{
                  $or: [
                      {$nor: [{a: {$exists: true}}]},
                      {$and: [{$alwaysTrue: 1}, {a: {$_internalSchemaType: ['number']}}]}
                  ]
              }]
        }]})"));
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
    ASSERT_SERIALIZES_TO(result.getValue(), fromjson("{$and: [{$alwaysTrue: 1}]}"));

    schema = fromjson("{properties: {a: {maxItems: 1}}}");
    result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());

    ASSERT_SERIALIZES_TO(result.getValue(), fromjson(R"(
        {$and: [{
              $and: [{
                  $or: [
                      {$nor: [{a: {$exists: true}}]},
                      {
                        $and: [{
                            $or: [
                                {$nor: [{a: {$_internalSchemaType: [4]}}]},
                                {a: {$_internalSchemaMaxItems: 1}}
                            ]
                        }]
                      }
                  ]
              }]
        }]})"));
}

TEST(JSONSchemaParserTest, MaxItemsTranslatesCorrectlyWithArrayType) {
    auto schema = fromjson("{properties: {a: {maxItems: 1, type: 'array'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue(), fromjson(R"(
        {$and: [{
              $and: [{
                  $or: [
                      {$nor: [{a: {$exists: true}}]},
                      {$and: [{a: {$_internalSchemaMaxItems: 1}}, {a: {$_internalSchemaType: [4]}}]}
                  ]
              }]
        }]})"));
}

TEST(JSONSchemaParserTest, MaxItemsTranslatesCorrectlyWithNonArrayType) {
    auto schema = fromjson("{properties: {a: {maxItems: 1, type: 'string'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue(), fromjson(R"(
    {$and: [{
            $and: [{
                $or: [
                    {$nor: [{a: {$exists: true}}]},
                    {$and: [{$alwaysTrue: 1}, {a: {$_internalSchemaType: [2]}}]}
                ]
            }]
    }]})"));
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
    ASSERT_SERIALIZES_TO(
        result.getValue().get(),
        fromjson("{$and: [{$and: [{bar: {$exists: true}}, {foo: {$exists: true}}]}]}"));
}

TEST(JSONSchemaParserTest, TopLevelRequiredTranslatesCorrectlyWithProperties) {
    BSONObj schema = fromjson("{required: ['foo'], properties: {foo: {type: 'number'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {$and: [
            {$and: [{$and: [{foo: {$_internalSchemaType: ['number']}}]}]},
            {$and: [{foo: {$exists: true}}]}
        ]
    })"));
}

TEST(JSONSchemaParserTest, RequiredTranslatesCorrectlyInsideProperties) {
    BSONObj schema = fromjson("{properties: {x: {required: ['y']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {
            $and: [{
                $and: [{
                    $or: [
                        {$nor: [{x: {$exists: true}}]},
                        {
                          $and: [{
                              $or: [
                                  {$nor: [{x: {$_internalSchemaType: [3]}}]},
                                  {
                                    $and:
                                        [{x: {$_internalSchemaObjectMatch: {y: {$exists: true}}}}]
                                  }
                              ]
                          }]
                        }
                    ]
                }]
            }]
        }
    )"));
}

TEST(JSONSchemaParserTest, RequiredTranslatesCorrectlyInsidePropertiesWithSiblingProperties) {
    BSONObj schema =
        fromjson("{properties: {x:{required: ['y'], properties: {y: {type: 'number'}}}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {
            $and: [{
                $and: [{
                    $or: [
                        {$nor: [{x: {$exists: true}}]},
                        {
                          $and: [
                              {
                                $or: [
                                    {$nor: [{x: {$_internalSchemaType: [3]}}]},
                                    {
                                      x: {
                                          $_internalSchemaObjectMatch:
                                           {$and: [{$and: [{y:
                                             {$_internalSchemaType: ['number']}}]}]}
                                      }
                                    }
                                ]
                              },
                              {
                                $or: [
                                    {$nor: [{x: {$_internalSchemaType: [3]}}]},
                                    {
                                      $and: [{
                                          x: {$_internalSchemaObjectMatch: {y: {$exists: true}}}
                                      }]
                                    }
                                ]
                              }
                          ]
                        }
                    ]
                }]
            }]
        }
    )"));
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
        typeResult.getValue()->serialize(&typeBuilder);

        BSONObjBuilder bsonTypeBuilder;
        bsonTypeResult.getValue()->serialize(&bsonTypeBuilder);

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
    ASSERT_SERIALIZES_TO(result.getValue().get(),
                         fromjson("{$and: [{$_internalSchemaMinProperties: 0}]}"));
}

TEST(JSONSchemaParserTest, TopLevelMaxPropertiesTranslatesCorrectly) {
    BSONObj schema = fromjson("{maxProperties: 0}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue().get(),
                         fromjson("{$and: [{$_internalSchemaMaxProperties: 0}]}"));
}

TEST(JSONSchemaParserTest, NestedMinPropertiesTranslatesCorrectly) {
    BSONObj schema =
        fromjson("{properties: {obj: {type: 'object', minProperties: 2}}, required: ['obj']}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {
            $and: [
                {
                  $and: [{
                      $and: [
                          {obj: {$_internalSchemaObjectMatch: {$_internalSchemaMinProperties: 2}}},
                          {obj: {$_internalSchemaType: [3]}}
                      ]
                  }]
                },
                {$and: [{obj: {$exists: true}}]}
            ]
        }
    )"));
}

TEST(JSONSchemaParserTest, NestedMaxPropertiesTranslatesCorrectly) {
    BSONObj schema =
        fromjson("{properties: {obj: {type: 'object', maxProperties: 2}}, required: ['obj']}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {
            $and: [
                {
                  $and: [{
                      $and: [
                          {obj: {$_internalSchemaObjectMatch: {$_internalSchemaMaxProperties: 2}}},
                          {obj: {$_internalSchemaType: [3]}}
                      ]
                  }]
                },
                {$and: [{obj: {$exists: true}}]}
            ]
        }
    )"));
}

TEST(JSONSchemaParserTest, NestedMinPropertiesTranslatesCorrectlyWithoutRequired) {
    BSONObj schema = fromjson("{properties: {obj: {type: 'object', minProperties: 2}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {
            $and: [{
                $and: [{
                    $or: [
                        {$nor: [{obj: {$exists: true}}]},
                        {
                          $and: [
                              {obj:
                                {$_internalSchemaObjectMatch: {$_internalSchemaMinProperties: 2}}},
                              {obj: {$_internalSchemaType: [3]}}
                          ]
                        }
                    ]
                }]
            }]
        }
    )"));
}

TEST(JSONSchemaParserTest, NestedMaxPropertiesTranslatesCorrectlyWithoutRequired) {
    BSONObj schema = fromjson("{properties: {obj: {type: 'object', maxProperties: 2}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {
            $and: [{
                $and: [{
                    $or: [
                        {$nor: [{obj: {$exists: true}}]},
                        {
                          $and: [
                              {obj:
                                {$_internalSchemaObjectMatch: {$_internalSchemaMaxProperties: 2}}},
                              {obj: {$_internalSchemaType: [3]}}
                          ]
                        }
                    ]
                }]
            }]
        }
    )"));
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

TEST(JSONSchemaParserTest, FailsToParseIfBsonTypeArrayContainsUnknownAlias) {
    BSONObj schema = fromjson("{properties: {obj: {bsonType: ['unknown']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, CanTranslateTopLevelTypeArrayWithoutObject) {
    BSONObj schema = fromjson("{type: ['number', 'string']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson("{$alwaysFalse: 1}"));
}

TEST(JSONSchemaParserTest, CanTranslateTopLevelBsonTypeArrayWithoutObject) {
    BSONObj schema = fromjson("{bsonType: ['number', 'string']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson("{$alwaysFalse: 1}"));
}

TEST(JSONSchemaParserTest, CanTranslateTopLevelTypeArrayWithObject) {
    BSONObj schema = fromjson("{type: ['number', 'object']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson("{}"));
}

TEST(JSONSchemaParserTest, CanTranslateTopLevelBsonTypeArrayWithObject) {
    BSONObj schema = fromjson("{bsonType: ['number', 'object']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson("{}"));
}

TEST(JSONSchemaParserTest, CanTranslateNestedTypeArray) {
    BSONObj schema = fromjson("{properties: {a: {type: ['number', 'object']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {
            $and: [{
                $and: [{
                    $or: [
                        {$nor: [{a: {$exists: true}}]},
                        {$and: [{a: {$_internalSchemaType: ['number', 3]}}]}
                    ]
                }]
            }]
        }
    )"));
}

TEST(JSONSchemaParserTest, CanTranslateNestedBsonTypeArray) {
    BSONObj schema = fromjson("{properties: {a: {bsonType: ['number', 'objectId']}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {
            $and: [{
                $and: [{
                    $or: [
                        {$nor: [{a: {$exists: true}}]},
                        {$and: [{a: {$_internalSchemaType: ['number', 7]}}]}
                    ]
                }]
            }]
        }
    )"));
}

TEST(JSONSchemaParserTest, DependenciesFailsToParseIfNotAnObject) {
    BSONObj schema = fromjson("{dependencies: []}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(JSONSchemaParserTest, DependenciesFailsToParseIfTheEmptyObject) {
    BSONObj schema = fromjson("{dependencies: {}}");
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
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {
            $and: [{
                $and: [{
                    $_internalSchemaCond: [
                        {a: {$exists: true}},
                        {
                          $and: [{
                              $and: [{
                                  $or: [
                                      {$nor: [{b: {$exists: true}}]},
                                      {$and: [{b: {$_internalSchemaType: [2]}}]}
                                  ]
                              }]
                          }]
                        },
                        {$alwaysTrue: 1}
                    ]
                }]
            }]
        }
    )"));
}

TEST(JSONSchemaParserTest, TopLevelPropertyDependencyTranslatesCorrectly) {
    BSONObj schema = fromjson("{dependencies: {a: ['b', 'c']}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {
            $and: [{
                $and: [{
                    $_internalSchemaCond: [
                        {a: {$exists: true}},
                        {$and: [{b: {$exists: true}}, {c: {$exists: true}}]},
                        {$alwaysTrue: 1}
                    ]
                }]
            }]
        }
    )"));
}

TEST(JSONSchemaParserTest, NestedSchemaDependencyTranslatesCorrectly) {
    BSONObj schema =
        fromjson("{properties: {a: {dependencies: {b: {properties: {c: {type: 'object'}}}}}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {$and: [{$and: [{
            $or: [
                {$nor: [{a: {$exists: true}}]},
                {
                  $and: [{$and: [{
                      $_internalSchemaCond: [
                          {a: {$_internalSchemaObjectMatch: {b: {$exists: true}}}},
                          {
                            $and: [{
                                $or: [
                                    {$nor: [{a: {$_internalSchemaType: [3]}}]},
                                    {
                                      a: {
                                          $_internalSchemaObjectMatch: {
                                              $and: [{
                                                  $or: [
                                                      {$nor: [{c: {$exists: true}}]},
                                                      {
                                                        $and: [{
                                                            c: {
                                                                $_internalSchemaType: [3]
                                                            }
                                                        }]
                                                      }
                                                  ]
                                              }]
                                          }
                                      }
                                    }
                                ]
                            }]
                          },
                          {$alwaysTrue: 1}
                      ]
                  }]}]
                }
            ]
        }]
    }]})"));
}

TEST(JSONSchemaParserTest, NestedPropertyDependencyTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {a: {dependencies: {b: ['c', 'd']}}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_SERIALIZES_TO(result.getValue().get(), fromjson(R"(
        {$and: [{$and: [{
            $or: [
                {$nor: [{a: {$exists: true}}]},
                {
                  $and: [{
                      $and: [{
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
                      }]
                  }]
                }
            ]
        }]
    }]})"));
}

}  // namespace
}  // namespace mongo
