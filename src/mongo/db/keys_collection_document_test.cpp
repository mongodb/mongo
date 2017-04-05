/**
 *    Copyright (C) 2017 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/keys_collection_document.h"
#include "mongo/unittest/unittest.h"

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

    KeysCollectionDocument keysCollectionDoc(
        keyId, std::move(purpose), std::move(key), std::move(expiresAt));

    auto serializedObj = keysCollectionDoc.toBSON();

    auto parseStatus = KeysCollectionDocument::fromBSON(serializedObj);
    ASSERT_OK(parseStatus.getStatus());
    const auto& parsedKey = parseStatus.getValue();

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
    auto status = KeysCollectionDocument::fromBSON(serializedObj).getStatus();
    ASSERT_EQ(ErrorCodes::NoSuchKey, status);
}

TEST(KeysCollectionDocument, MissingPurposeShouldFailToParse) {
    long long keyId = 10;

    SHA1Block::HashType keyHash;
    keyHash.fill(0);
    TimeProofService::Key key(keyHash);

    const auto expiresAt = LogicalTime(Timestamp(100, 200));

    BSONObjBuilder builder;
    builder.append("keyId", keyId);
    builder.append("key", BSONBinData(key.data(), key.size(), BinDataGeneral));
    expiresAt.asTimestamp().append(builder.bb(), "expiresAt");

    auto serializedObj = builder.done();
    auto status = KeysCollectionDocument::fromBSON(serializedObj).getStatus();
    ASSERT_EQ(ErrorCodes::NoSuchKey, status);
}

TEST(KeysCollectionDocument, MissingKeyShouldFailToParse) {
    long long keyId = 10;

    std::string purpose("signLogicalTime");

    const auto expiresAt = LogicalTime(Timestamp(100, 200));

    BSONObjBuilder builder;
    builder.append("keyId", keyId);
    builder.append("purpose", purpose);
    expiresAt.asTimestamp().append(builder.bb(), "expiresAt");

    auto serializedObj = builder.done();
    auto status = KeysCollectionDocument::fromBSON(serializedObj).getStatus();
    ASSERT_EQ(ErrorCodes::NoSuchKey, status);
}

TEST(KeysCollectionDocument, MissingExpiresAtShouldFailToParse) {
    long long keyId = 10;

    std::string purpose("signLogicalTime");

    SHA1Block::HashType keyHash;
    keyHash.fill(0);
    TimeProofService::Key key(keyHash);

    BSONObjBuilder builder;
    builder.append("keyId", keyId);
    builder.append("purpose", purpose);
    builder.append("key", BSONBinData(key.data(), key.size(), BinDataGeneral));

    auto serializedObj = builder.done();
    auto status = KeysCollectionDocument::fromBSON(serializedObj).getStatus();
    ASSERT_EQ(ErrorCodes::NoSuchKey, status);
}

}  // namespace
}  // namespace mongo
