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
    BSONObj schema = fromjson("{type: 1}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserTest, FailsToParseUnknownKeyword) {
    BSONObj schema = fromjson("{unknown: 1}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserTest, FailsToParseIfPropertiesIsNotAnObject) {
    BSONObj schema = fromjson("{properties: 1}}");
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

TEST(JSONSchemaParserTest, FailsToParseIfMaximumIsNotANumber) {
    BSONObj schema = fromjson("{maximum: 'foo'}");
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
                 "{a: {$_internalSchemaObjectMatch: {$and: [{$and:[{b: {$type:2}}]}]}}},"
                 "{a: {$type: 3}}]}]}]}"));
}

TEST(JSONSchemaParserTest, TopLevelNonObjectTypeTranslatesCorrectly) {
    BSONObj schema = fromjson("{type: 'string'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    // TODO SERVER-30028: Serialize to a new internal "always false" expression.
    ASSERT_BSONOBJ_EQ(builder.obj(), fromjson("{'': {$all: []}}"));
}

TEST(JSONSchemaParserTest, TypeNumberTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {num: {type: 'number'}}}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(builder.obj(),
                      fromjson("{$and: [{$and: [{$and: [{num: {$type: 'number'}}]}]}]}"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithTypeNumber) {
    BSONObj schema = fromjson("{properties: {num: {type: 'number', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(
        builder.obj(),
        fromjson("{$and: [{$and: [{$and: [{num: {$lte: 0}}, {num: {$type: 'number'}}]}]}]}"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithTypeLong) {
    BSONObj schema = fromjson("{properties: {num: {type: 'long', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(
        builder.obj(),
        fromjson("{$and: [{$and: [{$and: [{num: {$lte: 0}}, {num: {$type: 18}}]}]}]}"));
}

TEST(JSONSchemaParserTest, MaximumTranslatesCorrectlyWithTypeString) {
    BSONObj schema = fromjson("{properties: {num: {type: 'string', maximum: 0}}, type: 'object'}");
    auto result = JSONSchemaParser::parse(schema);
    ASSERT_OK(result.getStatus());
    BSONObjBuilder builder;
    result.getValue()->serialize(&builder);
    ASSERT_BSONOBJ_EQ(builder.obj(),
                      fromjson("{$and: [{$and: [{$and: [{}, {num: {$type: 2}}]}]}]}"));
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

}  // namespace
}  // namespace mongo
