// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/schema/assert_serializes_to.h"
#include "mongo/db/query/compiler/parsers/matcher/schema/json_schema_parser.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <utility>

namespace mongo {
namespace {

TEST(JSONSchemaParserEncryptTest, EncryptTranslatesCorrectly) {
    BSONObj schema = fromjson("{properties: {foo: {encrypt: {}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
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
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
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
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
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
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
    ASSERT_SERIALIZES_TO(optimizedResult, BSON(AlwaysFalseMatchExpression::kName << 1));
}

TEST(JSONSchemaParserEncryptTest, NestedEncryptTranslatesCorrectly) {
    BSONObj schema =
        fromjson("{properties: {a: {type: 'object', properties: {b: {encrypt: {}}}}}}");
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
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
    auto optimizedResult = optimizeMatchExpression(std::move(result.getValue()));
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
    BSONObj schema =
        BSON("properties" << BSON("foo" << BSON("encrypt" << BSON("bsonType" << "int"))));
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
                 "foo" << BSON("encrypt" << BSON("bsonType" << BSON_ARRAY("int" << "date"
                                                                                << "string")))));
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
}

TEST(JSONSchemaParserEncryptTest, ParseSucceedsIfEncryptFieldsAreValid) {
    auto schema =
        BSON("properties" << BSON(
                 "foo" << BSON("encrypt" << BSON(
                                   "bsonType" << "string"
                                              << "keyId"
                                              << "/pointer"
                                              << "algorithm"
                                              << "AEAD_AES_256_CBC_HMAC_SHA_512-Deterministic"))));
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_OK(result.getStatus());
}

TEST(JSONSchemaParserEncryptTest, FailsToParseIfEncryptHasBadFieldName) {
    BSONObj schema =
        BSON("properties" << BSON("foo" << BSON("encrypt" << BSON("keyIdx" << "/pointer"))));
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::IDLUnknownField);
    schema = BSON("properties" << BSON("foo" << BSON("encrypt" << BSON("bsonType" << "bool"
                                                                                  << "keyIdx"
                                                                                  << "/pointer"))));
    result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::IDLUnknownField);
}

TEST(JSONSchemaParserEncryptTest, FailsToParseWithBadKeyIdArray) {
    auto schema =
        BSON("properties" << BSON(
                 "foo" << BSON("encrypt" << BSON("keyId" << BSON_ARRAY("nonsense" << "again")))));
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus().code(), 51088);
}

TEST(JSONSchemaParserEncryptTest, FailsToParseWithBadBSONType) {
    auto schema =
        BSON("properties" << BSON("foo" << BSON("encrypt" << BSON("bsonType" << "Stringx"))));
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);

    schema =
        BSON("properties" << BSON(
                 "foo" << BSON("encrypt" << BSON(
                                   "bsonType" << (stdx::to_underlying(BSONType::jsTypeMax) + 1)))));
    result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::TypeMismatch);
}

TEST(JSONSchemaParserEncryptTest, FailsToParseWithBadAlgorithm) {
    auto schema =
        BSON("properties" << BSON("foo" << BSON("encrypt" << BSON("algorithm" << "Stringx"))));
    auto result = JSONSchemaParser::parse(new ExpressionContextForTest(), schema);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
}

TEST(JSONSchemaParserEncryptTest, FailsToParseWithBadPointer) {
    auto schema =
        BSON("properties" << BSON("foo" << BSON("encrypt" << BSON("keyId" << "invalidPointer"))));
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
