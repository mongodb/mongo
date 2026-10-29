// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
