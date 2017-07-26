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
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(builder.obj(), fromjson("{}"));
}

TEST(JSONSchemaParserTest, TypeObjectTranslatesCorrectly) {
    BSONObj schema = fromjson("{type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(builder.obj(), fromjson("{}"));
}

TEST(JSONSchemaParserTest, NestedTypeObjectTranslatesCorrectly) {
    BSONObj schema =
        fromjson("{properties: {a: {type: 'object', properties: {b: {type: 'string'}}}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(
        builder.obj(),
        fromjson("{$and: [{$and: [{$and: ["
                 "{$or: [{$nor: [{a: {$type: 3}}]}, {a: {$_internalSchemaObjectMatch:"
                 "{$and: [{$and:[{$or: [{$nor: [{b: {$exists: true}}]}, {b: {$type: 2}}]}]}]}}}]},"
                 "{$or: [{$nor: [{a: {$exists: true}}]}, {a: {$type: 3}}]}]}]}]}"));
}

TEST(JSONSchemaParserTest, TopLevelNonObjectTypeTranslatesCorrectly) {
    BSONObj schema = fromjson("{type: 'string'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(builder.obj(), fromjson("{$alwaysFalse: 1}"));
}

TEST(JSONSchemaParserTest, TypeNumberTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {num: {type: 'number'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(
        builder.obj(),
        fromjson("{$and: [{$and: [{$and: ["
                 "{$or: [{$nor: [{num: {$exists: true}}]}, {num: {$type: 'number'}}]}]}]}]}"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithTypeNumber) {
    BSONObj schema = fromjson("{properties: {num: {type: 'number', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(builder.obj(),
                      fromjson("{$and: [{$and: [{$and: ["
                               "{$or: [{$nor: [{num: {$type: 'number'}}]}, {num: {$lte: 0}}]},"
                               "{$or: [{$nor: [{num: {$exists: true}}]}, {num: {$type: 'number'}}]}"
                               "]}]}]}"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithTypeLong) {
    BSONObj schema = fromjson("{properties: {num: {type: 'long', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(builder.obj(),
                      fromjson("{$and: [{$and: [{$and: ["
                               "{$or: [{$nor: [{num: {$type: 'number'}}]}, {num: {$lte: 0}}]},"
                               "{$or: [{$nor: [{num: {$exists: true}}]}, {num: {$type: 18}}]}"
                               "]}]}]}"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithTypeString) {
    BSONObj schema = fromjson("{properties: {num: {type: 'string', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(builder.obj(),
                      fromjson("{$and: [{$and: [{$and: [{$alwaysTrue: 1},"
                               "{$or: [{$nor: [{num: {$exists: true}}]}, {num: {$type: 2}}]}"
                               "]}]}]}"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithNoType) {
    BSONObj schema = fromjson("{properties: {num: {maximum: 0}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(
        builder.obj(),
        fromjson("{$and: [{$and: [{$and: ["
                 "{$or: [{$nor: [{num: {$type: 'number'}}]}, {num: {$lte: 0}}]}]}]}]}"));
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
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(builder.obj(),
                      fromjson("{$and: [{$and: [{$and: ["
                               "{$or: [{$nor: [{num: {$type: 'number'}}]}, {num: {$gte: 0}}]},"
                               "{$or: [{$nor: [{num: {$exists: true}}]}, {num: {$type: 'number'}}]}"
                               "]}]}]}"));
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
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(
        builder.obj(),
        fromjson("{ $and: [ { $and: [ { $and: [ { $or: [ { $nor: [ { foo: { $type: 2 } } ] }, { "
                 "foo: { $_internalSchemaMaxLength: 5 } } ] }, { $or: [ { $nor: [ { foo: { "
                 "$exists: true } } ] }, { foo: { $type: 2 } } ] } ] } ] } ] }"));
}

TEST(JSONSchemaParserTest, MaxLengthTranslatesCorrectlyWithTypeString) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', maxLength: 5}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(
        builder.obj(),
        fromjson("{ $and: [ { $and: [ { $and: [ { $or: [ { $nor: [ { foo: { $type: 2 } } ] }, { "
                 "foo: { $_internalSchemaMaxLength: 5 } } ] }, { $or: [ { $nor: [ { foo: { "
                 "$exists: true } } ] }, { foo: { $type: 2 } } ] } ] } ] } ] }"));
}

TEST(JSONSchemaParserTest, MinimumTranslatesCorrectlyWithTypeLong) {
    BSONObj schema = fromjson("{properties: {num: {type: 'long', minimum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(builder.obj(),
                      fromjson("{$and: [{$and: [{$and: ["
                               "{$or: [{$nor: [{num: {$type: 'number'}}]}, {num: {$gte: 0}}]},"
                               "{$or: [{$nor: [{num: {$exists: true}}]}, {num: {$type: 18}}]}"
                               "]}]}]}"));
}

TEST(JSONSchemaParserTest, MinimumTranslatesCorrectlyWithTypeString) {
    BSONObj schema = fromjson("{properties: {num: {type: 'string', minimum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(builder.obj(),
                      fromjson("{$and: [{$and: [{$and: [{$alwaysTrue: 1},"
                               "{$or: [{$nor: [{num: {$exists: true}}]}, {num: {$type: 2}}]}"
                               "]}]}]}"));
}

TEST(JSONSchemaParserTest, MinimumTranslatesCorrectlyWithNoType) {
    BSONObj schema = fromjson("{properties: {num: {minimum: 0}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(
        builder.obj(),
        fromjson("{$and: [{$and: [{$and: ["
                 "{$or: [{$nor: [{num: {$type: 'number'}}]}, {num: {$gte: 0}}]}]}]}]}"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithExclusiveMaximumTrue) {
    BSONObj schema = fromjson(
        "{properties: {num: {type: 'long', maximum: 0, exclusiveMaximum: true}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(builder.obj(),
                      fromjson("{ $and: [ { $and: [ { $and: [ { $or: [ { $nor: [ { "
                               "num: { $type: 'number' } } ] }, { num: { $lt: 0 } } "
                               "] }, { $or: [ { $nor: [ { num: { $exists: true } } "
                               "] }, { num: { $type: 18 } } ] } ] } ] } ] }"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithExclusiveMaximumFalse) {
    BSONObj schema = fromjson(
        "{properties: {num: {type: 'long', maximum: 0, exclusiveMaximum: false}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(builder.obj(),
                      fromjson("{ $and: [ { $and: [ { $and: [ { $or: [ { $nor: [ { "
                               "num: { $type: 'number' } } ] }, { num: { $lte: 0 } "
                               "} ] }, { $or: [ { $nor: [ { num: { $exists: true } "
                               "} ] }, { num: { $type: 18 } } ] } ] } ] } ] }"));
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
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(builder.obj(),
                      fromjson("{$and: [{$and: [{$and: ["
                               "{$or: [{$nor: [{num: {$type: 'number'}}]}, {num: {$gt: 0}}]},"
                               "{$or: [{$nor: [{num: {$exists: true}}]}, {num: {$type: 18}}]}"
                               "]}]}]}"));
}

TEST(JSONSchemaParserTest, MinimumTranslatesCorrectlyWithExclusiveMinimumFalse) {
    BSONObj schema = fromjson(
        "{properties: {num: {type: 'long', minimum: 0, exclusiveMinimum: false}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(builder.obj(),
                      fromjson("{$and: [{$and: [{$and: ["
                               "{$or: [{$nor: [{num: {$type: 'number'}}]}, {num: {$gte: 0}}]},"
                               "{$or: [{$nor: [{num: {$exists: true}}]}, {num: {$type: 18}}]}"
                               "]}]}]}"));
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
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(
        builder.obj(),
        fromjson("{ $and: [ { $and: [ { $and: [ { $or: [ { $nor: [ { foo: { $type: 2 } } ] }, { "
                 "foo: { $_internalSchemaMinLength: 5 } } ] }, { $or: [ { $nor: [ { foo: { "
                 "$exists: true } } ] }, { foo: { $type: 2 } } ] } ] } ] } ] }"));
}

TEST(JSONSchemaParserTest, MinLengthTranslatesCorrectlyWithIntegralDouble) {
    BSONObj schema =
        fromjson("{properties: {foo: {type: 'string', minLength: 5.0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(
        builder.obj(),
        fromjson("{ $and: [ { $and: [ { $and: [ { $or: [ { $nor: [ { foo: { $type: 2 } } ] }, { "
                 "foo: { $_internalSchemaMinLength: 5 } } ] }, { $or: [ { $nor: [ { foo: { "
                 "$exists: true } } ] }, { foo: { $type: 2 } } ] } ] } ] } ] }"));
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
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);

    BSONObj expected = BSON(
        "$and" << BSON_ARRAY(BSON(
            "$and" << BSON_ARRAY(BSON(
                "$and" << BSON_ARRAY(
                    BSON("$or" << BSON_ARRAY(
                             BSON("$nor" << BSON_ARRAY(BSON("foo" << BSON("$type" << 2))))
                             << BSON("foo" << BSON("$regex"
                                                   << "abc"))))
                    << BSON("$or" << BSON_ARRAY(
                                BSON("$nor" << BSON_ARRAY(BSON("foo" << BSON("$exists" << true))))
                                << BSON("foo" << BSON("$type" << 2))))))))));

    ASSERT_BSONOBJ_EQ(builder.obj(), expected);
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
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(
        builder.obj(),
        fromjson("{$and: [{$and: [{$and: [{$or: [{$nor: [{foo: {$type: 'number'}}]}, "
                 "{foo: {$_internalSchemaFmod: [NumberDecimal('5.3'), 0]}}]}, {$or: [{$nor: [{foo: "
                 "{$exists: true}}]}, {foo: {$type: 'number'}}]}]}]}]}"));
}

}  // namespace
}  // namespace mongo
