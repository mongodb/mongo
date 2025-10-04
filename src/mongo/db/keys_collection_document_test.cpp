/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/timestamp.h"
#include "mongo/crypto/hash_block.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string>

namespace mongo {
namespace {

TEST(KeysCollectionDocument, Roundtrip) {
    long long keyId = 10;

    const std::string purpose("signLogicalTime");

    SHA1Block::HashType keyHash;
    keyHash.fill(0);
    keyHash[19] = 6;
    keyHash[0] = 12;
    TimeProofService::Key key(keyHash);

    const auto expiresAt = LogicalTime(Timestamp(100, 200));

    KeysCollectionDocument keysCollectionDoc(keyId);
    keysCollectionDoc.setKeysCollectionDocumentBase({purpose, key, expiresAt});

    auto serializedObj = keysCollectionDoc.toBSON();
    auto parsedKey = KeysCollectionDocument::parse(serializedObj, IDLParserContext("keyDoc"));

    ASSERT_EQ(keyId, parsedKey.getKeyId());
    ASSERT_EQ(purpose, parsedKey.getPurpose());
    ASSERT_TRUE(key == parsedKey.getKey());
    ASSERT_EQ(expiresAt.asTimestamp(), parsedKey.getExpiresAt().asTimestamp());
}

TEST(KeysCollectionDocument, MissingKeyIdShouldFailToParse) {
    std::string purpose("signLogicalTime");

    SHA1Block::HashType keyHash;
    keyHash.fill(0);
    TimeProofService::Key key(keyHash);

    const auto expiresAt = LogicalTime(Timestamp(100, 200));

    BSONObjBuilder builder;
    builder.append("purpose", purpose);
    builder.append("key", BSONBinData(key.data(), key.size(), BinDataGeneral));
    expiresAt.asTimestamp().append(builder.bb(), "expiresAt");

    auto serializedObj = builder.done();
    ASSERT_THROWS_CODE(KeysCollectionDocument::parse(serializedObj, IDLParserContext("keyDoc")),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST(KeysCollectionDocument, MissingPurposeShouldFailToParse) {
    long long keyId = 10;

    SHA1Block::HashType keyHash;
    keyHash.fill(0);
    TimeProofService::Key key(keyHash);

    const auto expiresAt = LogicalTime(Timestamp(100, 200));

    BSONObjBuilder builder;
    builder.append("_id", keyId);
    builder.append("key", BSONBinData(key.data(), key.size(), BinDataGeneral));
    expiresAt.asTimestamp().append(builder.bb(), "expiresAt");

    auto serializedObj = builder.done();
    ASSERT_THROWS_CODE(KeysCollectionDocument::parse(serializedObj, IDLParserContext("keyDoc")),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST(KeysCollectionDocument, MissingKeyShouldFailToParse) {
    long long keyId = 10;

    std::string purpose("signLogicalTime");

    const auto expiresAt = LogicalTime(Timestamp(100, 200));

    BSONObjBuilder builder;
    builder.append("_id", keyId);
    builder.append("purpose", purpose);
    expiresAt.asTimestamp().append(builder.bb(), "expiresAt");

    auto serializedObj = builder.done();
    ASSERT_THROWS_CODE(KeysCollectionDocument::parse(serializedObj, IDLParserContext("keyDoc")),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST(KeysCollectionDocument, MissingExpiresAtShouldFailToParse) {
    long long keyId = 10;

    std::string purpose("signLogicalTime");

    SHA1Block::HashType keyHash;
    keyHash.fill(0);
    TimeProofService::Key key(keyHash);

    BSONObjBuilder builder;
    builder.append("_id", keyId);
    builder.append("purpose", purpose);
    builder.append("key", BSONBinData(key.data(), key.size(), BinDataGeneral));

    auto serializedObj = builder.done();
    ASSERT_THROWS_CODE(KeysCollectionDocument::parse(serializedObj, IDLParserContext("keyDoc")),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

}  // namespace
}  // namespace mongo
