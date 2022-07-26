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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/schema/encrypt_schema_gen.h"
#include "mongo/db/matcher/schema/encrypt_schema_types.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

TEST(EncryptSchemaTest, KeyIDTypePointerTest) {
    auto tempObj = BSON("pointer"
                        << "/pointer/pointing");
    auto elem = tempObj["pointer"];
    auto keyid = EncryptSchemaKeyId::parseFromBSON(elem);
    ASSERT(keyid.type() == EncryptSchemaKeyId::Type::kJSONPointer);
    ASSERT_FALSE(UUID::parse(keyid.jsonPointer().toString()).isOK());
}

TEST(EncryptSchemaTest, KeyIDTypeArrayTest) {
    auto tempObj =
        BSON("array" << BSON_ARRAY(UUID::gen() << UUID::gen() << UUID::gen() << UUID::gen()));
    auto elem = tempObj["array"];
    auto keyid = EncryptSchemaKeyId::parseFromBSON(elem);
    ASSERT(keyid.type() == EncryptSchemaKeyId::Type::kUUIDs);
}

TEST(EncryptSchemaTest, KeyIDTypeInvalidTest) {
    auto tempObj = BSON("invalid" << 5);
    auto elem = tempObj["invalid"];
    ASSERT_THROWS(EncryptSchemaKeyId::parseFromBSON(elem), DBException);
}

DEATH_TEST(EncryptSchemaTest, KeyIDPointerToBSON, "invariant") {
    BSONObjBuilder builder;
    EncryptSchemaKeyId pointerKeyID{"/pointer"};
    pointerKeyID.serializeToBSON("pointer", &builder);
    auto resultObj = builder.obj();
    BSONElement pointer = resultObj["pointer"];
    ASSERT(pointer);
    ASSERT_EQ(pointer.type(), BSONType::String);
    EncryptSchemaKeyId pointerParsed = EncryptSchemaKeyId::parseFromBSON(pointer);
    pointerParsed.uuids();
}

DEATH_TEST(EncryptSchemaTest, KeyIDArrayToBSON, "invariant") {
    BSONObjBuilder builder;
    std::vector<UUID> vect{UUID::gen(), UUID::gen()};
    EncryptSchemaKeyId vectKeyId{vect};
    vectKeyId.serializeToBSON("array", &builder);
    auto resultObj = builder.obj();
    BSONElement array = resultObj["array"];
    ASSERT(array);
    ASSERT_EQ(array.type(), BSONType::Array);
    ASSERT_EQ(array.Array().size(), unsigned{2});
    EncryptSchemaKeyId parsed = EncryptSchemaKeyId::parseFromBSON(array);
    parsed.jsonPointer();
}

TEST(EncryptSchemaTest, ParseFullEncryptObjectFromBSON) {
    BSONObj encryptInfoBSON = BSON("bsonType"
                                   << "int"
                                   << "algorithm"
                                   << "AEAD_AES_256_CBC_HMAC_SHA_512-Deterministic"
                                   << "keyId"
                                   << "/pointer");
    IDLParserContext ctxt("encrypt");
    auto encryptInfo = EncryptionInfo::parse(ctxt, encryptInfoBSON);
    MatcherTypeSet resultMatcherSet;
    resultMatcherSet.bsonTypes.insert(BSONType::NumberInt);
    ASSERT_TRUE(encryptInfo.getBsonType() == BSONTypeSet(resultMatcherSet));
    ASSERT_TRUE(encryptInfo.getAlgorithm().get() == FleAlgorithmEnum::kDeterministic);
    EncryptSchemaKeyId keyid = encryptInfo.getKeyId().get();
    ASSERT_TRUE(keyid.type() == EncryptSchemaKeyId::Type::kJSONPointer);
    ASSERT_EQ(keyid.jsonPointer().toString(), "/pointer");
}

TEST(EncryptSchemaTest, WrongTypeFailsParse) {
    BSONObj encryptInfoBSON = BSON("keyId" << 2);
    IDLParserContext ctxt("encrypt");
    ASSERT_THROWS_CODE(EncryptionInfo::parse(ctxt, encryptInfoBSON), DBException, 51085);
    encryptInfoBSON = BSON("algorithm"
                           << "garbage");
    ASSERT_THROWS_CODE(
        EncryptionInfo::parse(ctxt, encryptInfoBSON), DBException, ErrorCodes::BadValue);
}

}  // namespace
}  // namespace mongo
