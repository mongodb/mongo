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

#include "mongo/db/query/compiler/parsers/matcher/schema/encrypt_schema_types_parser.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/matcher/schema/encrypt_schema_gen.h"
#include "mongo/db/matcher/schema/encrypt_schema_types.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

TEST(EncryptSchemaTypesParserTest, KeyIDTypePointerTest) {
    auto tempObj = BSON("pointer" << "/pointer/pointing");
    auto elem = tempObj["pointer"];
    auto keyid = parsers::matcher::schema::parseEncryptSchemaKeyId(elem);
    ASSERT(keyid.type() == EncryptSchemaKeyId::Type::kJSONPointer);
    ASSERT_FALSE(UUID::parse(keyid.jsonPointer().toString()).isOK());
}

TEST(EncryptSchemaTypesParserTest, KeyIDTypeArrayTest) {
    auto tempObj =
        BSON("array" << BSON_ARRAY(UUID::gen() << UUID::gen() << UUID::gen() << UUID::gen()));
    auto elem = tempObj["array"];
    auto keyid = parsers::matcher::schema::parseEncryptSchemaKeyId(elem);
    ASSERT(keyid.type() == EncryptSchemaKeyId::Type::kUUIDs);
}

TEST(EncryptSchemaTypesParserTest, KeyIDTypeInvalidTest) {
    auto tempObj = BSON("invalid" << 5);
    auto elem = tempObj["invalid"];
    ASSERT_THROWS(parsers::matcher::schema::parseEncryptSchemaKeyId(elem), DBException);
}

DEATH_TEST(EncryptSchemaTypesParserTest, KeyIDPointerToBSON, "tassert") {
    BSONObjBuilder builder;
    EncryptSchemaKeyId pointerKeyID{"/pointer"};

    pointerKeyID.serializeToBSON("pointer", &builder);
    auto resultObj = builder.obj();
    ASSERT_BSONOBJ_EQ(resultObj, BSON("pointer" << "/pointer"));
    BSONElement pointer = resultObj["pointer"];
    ASSERT(pointer);
    ASSERT_EQ(pointer.type(), BSONType::string);

    EncryptSchemaKeyId parsedPointer = parsers::matcher::schema::parseEncryptSchemaKeyId(pointer);
    ASSERT_EQ(parsedPointer.type(), EncryptSchemaKeyId::Type::kJSONPointer);
    ASSERT_EQ(parsedPointer.jsonPointer().toString(), "/pointer");

    // Attempt to access as UUIDs. We expect this to tassert since it is the wrong type.
    parsedPointer.uuids();
}

DEATH_TEST(EncryptSchemaTypesParserTest, KeyIDArrayToBSON, "tassert") {
    BSONObjBuilder builder;
    std::vector<UUID> uuidList{UUID::gen(), UUID::gen()};
    EncryptSchemaKeyId vectKeyId{uuidList};

    vectKeyId.serializeToBSON("array", &builder);
    auto resultObj = builder.obj();
    ASSERT_BSONOBJ_EQ(resultObj, BSON("array" << BSON_ARRAY(uuidList[0] << uuidList[1])));
    BSONElement array = resultObj["array"];
    ASSERT(array);
    ASSERT_EQ(array.type(), BSONType::array);
    ASSERT_EQ(array.Array().size(), unsigned{2});

    EncryptSchemaKeyId parsedArray = parsers::matcher::schema::parseEncryptSchemaKeyId(array);
    ASSERT_EQ(parsedArray.type(), EncryptSchemaKeyId::Type::kUUIDs);
    std::vector<UUID> parsedUUIDs = parsedArray.uuids();
    ASSERT_EQ(parsedUUIDs[0], uuidList[0]);
    ASSERT_EQ(parsedUUIDs[1], uuidList[1]);

    // Attempt to access as a jsonPointer. We expect this to tassert since this is the wrong type.
    parsedArray.jsonPointer();
}

TEST(EncryptSchemaTypesParserTest, ParseFullEncryptObjectFromBSON) {
    BSONObj encryptInfoBSON = BSON("bsonType" << "int"
                                              << "algorithm"
                                              << "AEAD_AES_256_CBC_HMAC_SHA_512-Deterministic"
                                              << "keyId"
                                              << "/pointer");
    IDLParserContext ctxt("encrypt");
    auto encryptInfo = EncryptionInfo::parse(encryptInfoBSON, ctxt);
    MatcherTypeSet resultMatcherSet;
    resultMatcherSet.bsonTypes.insert(BSONType::numberInt);
    ASSERT_TRUE(encryptInfo.getBsonType() == BSONTypeSet(resultMatcherSet));
    ASSERT_TRUE(encryptInfo.getAlgorithm().value() == FleAlgorithmEnum::kDeterministic);
    EncryptSchemaKeyId keyid = encryptInfo.getKeyId().value();
    ASSERT_TRUE(keyid.type() == EncryptSchemaKeyId::Type::kJSONPointer);
    ASSERT_EQ(keyid.jsonPointer().toString(), "/pointer");
}

TEST(EncryptSchemaTypesParserTest, WrongTypeFailsParse) {
    BSONObj encryptInfoBSON = BSON("keyId" << 2);
    IDLParserContext ctxt("encrypt");
    ASSERT_THROWS_CODE(EncryptionInfo::parse(encryptInfoBSON, ctxt), DBException, 51085);
    encryptInfoBSON = BSON("algorithm" << "garbage");
    ASSERT_THROWS_CODE(
        EncryptionInfo::parse(encryptInfoBSON, ctxt), DBException, ErrorCodes::BadValue);
}

}  // namespace
}  // namespace mongo
