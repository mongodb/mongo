// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/index_entry_comparison.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/hex.h"
#include "mongo/util/overloaded_visitor.h"

#include <memory>
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

void buildDupKeyErrorStatusProducesExpectedErrorObject(
    DuplicateKeyErrorInfo::FoundValue&& foundValue) {
    NamespaceString collNss = NamespaceString::createNamespaceString_forTest("test.foo");
    std::string indexName("a_1_b_1");
    auto keyPattern = BSON("a" << 1 << "b" << 1);
    auto keyValue = BSON("" << 10 << ""
                            << "abc");
    auto keyValueWithFieldName = BSON("a" << 10 << "b"
                                          << "abc");

    BSONObjBuilder expectedObjBuilder;
    expectedObjBuilder.append("keyPattern", keyPattern);
    expectedObjBuilder.append("keyValue", keyValueWithFieldName);
    visit(OverloadedVisitor{
              [](std::monostate) {},
              [&](const RecordId& rid) { rid.serializeToken("foundValue", &expectedObjBuilder); },
              [&](const BSONObj& obj) { expectedObjBuilder.append("foundValue", obj); },
          },
          foundValue);
    auto expectedObj = expectedObjBuilder.obj();

    auto dupKeyStatus = buildDupKeyErrorStatus(
        keyValue, collNss, indexName, keyPattern, BSONObj{}, std::move(foundValue));
    ASSERT_NOT_OK(dupKeyStatus);
    EXPECT_EQ(dupKeyStatus.code(), ErrorCodes::DuplicateKey);

    auto extraInfo = dupKeyStatus.extraInfo<DuplicateKeyErrorInfo>();
    ASSERT(extraInfo);

    ASSERT_BSONOBJ_EQ(extraInfo->getKeyPattern(), keyPattern);
    ASSERT_BSONOBJ_EQ(extraInfo->getDuplicatedKeyValue(), keyValueWithFieldName);

    BSONObjBuilder objBuilder;
    extraInfo->serialize(&objBuilder);
    auto obj = objBuilder.obj();
    ASSERT_BSONOBJ_EQ(obj, expectedObj);

    // Ensure the object is the same after parsing and serializing again.
    auto parsedExtraInfo =
        std::dynamic_pointer_cast<const DuplicateKeyErrorInfo>(DuplicateKeyErrorInfo::parse(obj));
    BSONObjBuilder afterParseObjBuilder;
    parsedExtraInfo->serialize(&afterParseObjBuilder);
    ASSERT_BSONOBJ_EQ(afterParseObjBuilder.obj(), expectedObj);
}

TEST(IndexEntryComparison, BuildDupKeyErrorStatusProducesExpectedErrorObject) {
    buildDupKeyErrorStatusProducesExpectedErrorObject(std::monostate{});
    buildDupKeyErrorStatusProducesExpectedErrorObject(RecordId{1});
    buildDupKeyErrorStatusProducesExpectedErrorObject(BSON("c" << 1));
}

void duplicateKeyErrorSerializationAndParseReturnTheSameObject(
    BSONObj keyPattern,
    BSONObj keyValue,
    BSONObj collation,
    BSONObj keyValueWithFieldName,
    BSONObj expectedEncodedKeyValueField) {
    NamespaceString collNss = NamespaceString::createNamespaceString_forTest("test.foo");
    std::string indexName("a_1_b_1");

    auto dupKeyStatus = buildDupKeyErrorStatus(keyValue, collNss, indexName, keyPattern, collation);
    auto extraInfo = dupKeyStatus.extraInfo<DuplicateKeyErrorInfo>();
    ASSERT(extraInfo);
    ASSERT_BSONOBJ_EQ(extraInfo->getDuplicatedKeyValue(), keyValueWithFieldName);

    // Build a serialized representation of the 'DuplicateKeyErrorInfo'.
    BSONObjBuilder objBuilder;
    extraInfo->serialize(&objBuilder);
    auto duplicateKeyErrorDoc = objBuilder.obj();
    ASSERT_BSONOBJ_EQ(duplicateKeyErrorDoc.getField("keyValue").Obj(),
                      expectedEncodedKeyValueField);

    // Verify that parsing of the serialized representation of the 'DuplicateKeyErrorInfo' object
    // results in an identical 'DuplicateKeyErrorInfo' object.
    auto parsedDuplicatedErrorInfo = std::dynamic_pointer_cast<const DuplicateKeyErrorInfo>(
        DuplicateKeyErrorInfo::parse(duplicateKeyErrorDoc));

    ASSERT(parsedDuplicatedErrorInfo.get());
    ASSERT_BSONOBJ_EQ(parsedDuplicatedErrorInfo->getKeyPattern(), keyPattern);
    ASSERT_BSONOBJ_EQ(parsedDuplicatedErrorInfo->getDuplicatedKeyValue(), keyValueWithFieldName);
}

TEST(IndexEntryComparison, BuildDupKeyErrorSerializeAndParseReturnTheSameObjectWithCollation) {
    auto keyPattern = BSON("a" << 1 << "b" << 1);
    auto str = "abc"sv;
    auto keyValue = BSON("" << 10 << "" << str);
    auto collation = BSON("x" << 'y');
    auto keyValueWithFieldName = BSON("a" << 10 << "b" << str);
    auto expectedEncodedKeyValueField = BSON("a" << 10 << "b" << hexblob::encodeLower(str));
    duplicateKeyErrorSerializationAndParseReturnTheSameObject(
        keyPattern, keyValue, collation, keyValueWithFieldName, expectedEncodedKeyValueField);
}

TEST(IndexEntryComparison, BuildDupKeyErrorSerializeAndParseReturnTheSameObjectForInvalidUtf8) {
    auto keyPattern = BSON("a" << 1 << "b" << 1);
    auto str = std::string_view("\xc3\x28");
    auto keyValue = BSON("" << 10 << "" << str);
    auto collation = BSONObj();
    auto keyValueWithFieldName = BSON("a" << 10 << "b" << str);
    auto expectedEncodedKeyValueField = BSON("a" << 10 << "b" << hexblob::encodeLower(str));
    duplicateKeyErrorSerializationAndParseReturnTheSameObject(
        keyPattern, keyValue, collation, keyValueWithFieldName, expectedEncodedKeyValueField);
}

TEST(IndexEntryComparison, BuildDupKeyErrorMessageIncludesCollationAndHexEncodedCollationKey) {
    std::string_view mockCollationKey("bar");

    NamespaceString collNss = NamespaceString::createNamespaceString_forTest("test.foo");
    std::string indexName("a_1");
    auto keyPattern = BSON("a" << 1);
    auto keyValue = BSON("" << mockCollationKey);
    auto collation = BSON("locale" << "en_US");

    auto dupKeyStatus = buildDupKeyErrorStatus(keyValue, collNss, indexName, keyPattern, collation);
    ASSERT_NOT_OK(dupKeyStatus);
    EXPECT_EQ(dupKeyStatus.code(), ErrorCodes::DuplicateKey);

    ASSERT(dupKeyStatus.reason().find("collation:") != std::string::npos);

    // Verify that the collation key is hex encoded in the error message.
    std::string expectedHexEncoding = "0x" + hexblob::encodeLower(mockCollationKey);
    ASSERT(dupKeyStatus.reason().find(expectedHexEncoding) != std::string::npos);

    // But no hex encoding should have taken place inside the key attached to the extra error info.
    auto extraInfo = dupKeyStatus.extraInfo<DuplicateKeyErrorInfo>();
    ASSERT(extraInfo);
    ASSERT_BSONOBJ_EQ(extraInfo->getKeyPattern(), keyPattern);
    ASSERT_BSONOBJ_EQ(extraInfo->getDuplicatedKeyValue(), BSON("a" << mockCollationKey));
}

TEST(IndexEntryComparison, BuildDupKeyErrorMessageHexEncodesInvalidUTF8ForIndexWithoutCollation) {
    NamespaceString collNss = NamespaceString::createNamespaceString_forTest("test.foo");
    std::string indexName("a_1");
    auto keyPattern = BSON("a" << 1);

    // The byte sequence c0 16 is invalid UTF-8 since this is an overlong encoding of the letter
    // "a", which should be represented as simply 0x16. The byte 0xc0 is always illegal in UTF-8
    // since it would only ever be used for an overload two-byte encoding of an ASCII character.
    auto keyValue = BSON("" << "\xc0\x16");
    auto dupKeyStatus = buildDupKeyErrorStatus(keyValue, collNss, indexName, keyPattern, BSONObj{});
    ASSERT_NOT_OK(dupKeyStatus);
    EXPECT_EQ(dupKeyStatus.code(), ErrorCodes::DuplicateKey);

    // We expect to find a hex-encoded version of the illegal UTF-8 byte sequence inside the error
    // string.
    ASSERT(dupKeyStatus.reason().find("0xc016") != std::string::npos);

    // In the extra error info, we expect that no hex encoding has taken place.
    auto extraInfo = dupKeyStatus.extraInfo<DuplicateKeyErrorInfo>();
    ASSERT(extraInfo);
    ASSERT_BSONOBJ_EQ(extraInfo->getKeyPattern(), keyPattern);
    ASSERT_BSONOBJ_EQ(extraInfo->getDuplicatedKeyValue(), BSON("a" << "\xc0\x16"));
}

}  // namespace mongo
