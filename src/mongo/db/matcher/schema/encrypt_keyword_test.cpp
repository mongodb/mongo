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

TEST(JSONSchemaParserEncryptTest, EncryptTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {foo: {encrypt: {}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"(
        {
        $or:
            [{foo: {$not: {$exists: true}}}, {
                $and:
                    [ {foo: {$_internalSchemaBinDataSubType: 6}}, {foo: {$_internalSchemaType: [5]}} ]
            }]
        })"));
}

TEST(JSONSchemaParserEncryptTest, EncryptWithSingleBsonTypeTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {foo: {encrypt: {bsonType: \"string\"}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"(
        {
        $or:
            [{foo: {$not: {$exists: true}}}, {
                $and:
                    [ {foo: {$_internalSchemaBinDataSubType: 6}},
                    {foo: {$_internalSchemaBinDataEncryptedType: [2]}},
                    {foo: {$_internalSchemaType: [5]}}]
            }]
        })"));
}

TEST(JSONSchemaParserEncryptTest, EncryptWithArrayOfMultipleTypesTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {foo: {encrypt: {bsonType: [\"string\",\"date\"]}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"(
        {
        $or:
            [{foo: {$not: {$exists: true}}}, {
                $and:
                    [ {foo: {$_internalSchemaBinDataSubType: 6}},
                    {foo: {$_internalSchemaBinDataEncryptedType: [2, 9]}},
                    {foo: {$_internalSchemaType: [5]}}]
            }]
        })"));
}

TEST(JSONSchemaParserEncryptTest, TopLevelEncryptTranslatesCorrectly) {
    BSONObj schema = fromjson("{encrypt: {}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, BSON(AlwaysFalseMatchExpression::kName << 1));
}

TEST(JSONSchemaParserEncryptTest, NestedEncryptTranslatesCorrectly) {
    BSONObj schema =
        fromjson("{properties: {a: {type: 'object', properties: {b: {encrypt: {}}}}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"(
        {
        $or:
            [{a: {$not: {$exists: true}}}, {
                $and: [
                    {
                      a: {
                          $_internalSchemaObjectMatch: {
                              $or: [
                                  {b: {$not : {$exists: true}}},
                                  {
                                    $and: [
                                        {b: {$_internalSchemaBinDataSubType: 6}},
                                        {b: {$_internalSchemaType: [5]}}
                                    ]
                                  }
                              ]
                          }
                      }
                    },
                    {a: {$_internalSchemaType: [3]}}
                ]
            }]
        })"));
}

TEST(JSONSchemaParserEncryptTest, NestedEncryptInArrayTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {a: {type: 'array', items: {encrypt: {}}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = MatchExpression::optimize(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, fromjson(R"(
        {
        $or:
            [{a: {$not: {$exists: true}}}, {
                $and: [
                    {
                      a: {
                          $_internalSchemaAllElemMatchFromIndex : [
                              0,
                              {
                                $and: [
                                    {i: {$_internalSchemaBinDataSubType: 6}},
                                    {i: {$_internalSchemaType: [5]}}
                                ]
                              }
                          ]
                      }
                    },
                    {a: {$_internalSchemaType: [4]}}
                ]
            }]
        })"));
}

TEST(JSONSchemaParserEncryptTest, FailsToParseIfBothEncryptAndTypeArePresent) {
    BSONObj schema = fromjson("{encrypt: {}, type: 'object'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserEncryptTest, FailsToParseIfBothEncryptAndBSONTypeArePresent) {
    BSONObj schema = fromjson("{encrypt: {}, bsonType: 'binData'}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserEncryptTest, FailsToParseIfEncryptValueIsNotObject) {
    BSONObj schema = fromjson("{properties: {foo: {encrypt: 12}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserEncryptTest, ParseSucceedsWithEmptyEncryptObject) {
    BSONObj schema = BSON("properties" << BSON("foo" << BSON("encrypt" << BSONObj())));
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
}

TEST(JSONSchemaParserEncryptTest, ParseSucceedsWithBsonType) {
    BSONObj schema = BSON("properties" << BSON("foo" << BSON("encrypt" << BSON("bsonType"
                                                                               << "int"))));
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
}

TEST(JSONSchemaParserEncryptTest, ParseFailsWithBsonTypeGivenByCode) {
    BSONObj schema = BSON("properties" << BSON("foo" << BSON("encrypt" << BSON("bsonType" << 5))));
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserEncryptTest, ParseSucceedsWithArrayOfBsonTypes) {
    BSONObj schema =
        BSON("properties" << BSON(
                 "foo" << BSON("encrypt" << BSON("bsonType" << BSON_ARRAY("int"
                                                                          << "date"
                                                                          << "string")))));
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
}

TEST(JSONSchemaParserEncryptTest, ParseSucceedsIfEncryptFieldsAreValid) {
    auto schema = BSON(
        "properties" << BSON(
            "foo" << BSON("encrypt" << BSON("bsonType"
                                            << "string"
                                            << "keyId"
                                            << "/pointer"
                                            << "algorithm"
                                            << "AEAD_AES_256_CBC_HMAC_SHA_512-Deterministic"))));
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
}

TEST(JSONSchemaParserEncryptTest, FailsToParseIfEncryptHasBadFieldName) {
    BSONObj schema = BSON("properties" << BSON("foo" << BSON("encrypt" << BSON("keyIdx"
                                                                               << "/pointer"))));
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus().code(), 40415);
    schema = BSON("properties" << BSON("foo" << BSON("encrypt" << BSON("bsonType"
                                                                       << "bool"
                                                                       << "keyIdx"
                                                                       << "/pointer"))));
    result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus().code(), 40415);
}

TEST(JSONSchemaParserEncryptTest, FailsToParseWithBadKeyIdArray) {
    auto schema = BSON(
        "properties" << BSON("foo" << BSON("encrypt" << BSON("keyId" << BSON_ARRAY("nonsense"
                                                                                   << "again")))));
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus().code(), 51088);
}

TEST(JSONSchemaParserEncryptTest, FailsToParseWithBadBSONType) {
    auto schema = BSON("properties" << BSON("foo" << BSON("encrypt" << BSON("bsonType"
                                                                            << "Stringx"))));
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);

    schema = BSON("properties" << BSON(
                      "foo" << BSON("encrypt" << BSON("bsonType" << (BSONType::JSTypeMax + 1)))));
    result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserEncryptTest, FailsToParseWithBadAlgorithm) {
    auto schema = BSON("properties" << BSON("foo" << BSON("encrypt" << BSON("algorithm"
                                                                            << "Stringx"))));
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
}

TEST(JSONSchemaParserEncryptTest, FailsToParseWithBadPointer) {
    auto schema = BSON("properties" << BSON("foo" << BSON("encrypt" << BSON("keyId"
                                                                            << "invalidPointer"))));
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus().code(), 51065);
}

TEST(JSONSchemaParserEncryptTest, TopLevelEncryptMetadataValidatedCorrectly) {
    BSONObj schema = fromjson(
        "{encryptMetadata: {algorithm: \"AEAD_AES_256_CBC_HMAC_SHA_512-Deterministic\","
        " keyId: [{$binary: \"ASNFZ4mrze/ty6mHZUMhAQ==\", $type: \"04\"}]}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
}

TEST(JSONSchemaParserEncryptTest, NestedEncryptMetadataValidatedCorrectly) {
    BSONObj schema = fromjson(
        "{properties: {a: {encryptMetadata: {algorithm: \"AEAD_AES_256_CBC_HMAC_SHA_512-Random\", "
        "keyId: [{$binary: \"ASNFZ4mrze/ty6mHZUMhAQ==\", $type: \"04\"}]}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
}

TEST(JSONSchemaParserEncryptTest, FailsToParseIfEncryptMetadataValueIsEmptyObject) {
    BSONObj schema = fromjson("{encryptMetadata: {}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserEncryptTest, FailsToParseIfBothEncryptAndEncryptMetadataAreSiblings) {
    BSONObj schema = fromjson(
        "{encrypt: {}, encryptMetadata: {algorithm: "
        "\"AEAD_AES_256_CBC_HMAC_SHA_512-Random\"}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(JSONSchemaParserEncryptTest, FailsToParseWithNonUUIDArrayElement) {
    BSONArrayBuilder builder;
    UUID::gen().appendToArrayBuilder(&builder);
    UUID::gen().appendToArrayBuilder(&builder);
    builder.appendBinData(16, BinDataType::Encrypt, "16charactershere");
    auto schema =
        BSON("properties" << BSON("foo" << BSON("encrypt" << BSON("keyId" << builder.arr()))));
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus().code(), 51084);
}

}  // namespace
}  // namespace mongo
