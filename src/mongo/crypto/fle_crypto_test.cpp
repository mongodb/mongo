/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/crypto/fle_crypto.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stack>
#include <string>
#include <tuple>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/config.h"
#include "mongo/db/matcher/schema/encrypt_schema_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/platform/decimal128.h"
#include "mongo/rpc/object_check.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/time_support.h"


// TODO (SERVER-63780) - remove once we stop using AEAD for ECC crypto with empty associated data
// Tomcrypt SHA implementation cannot accept it so we skip these tests for now
#ifdef MONGO_CONFIG_SSL

namespace mongo {

template <FLETokenType tt>
bool operator==(const FLEToken<tt>& left, const FLEToken<tt>& right) {
    return std::make_tuple(left.type, left.data) == std::make_tuple(right.type, right.data);
}

template <typename T>
std::string hexdump(const std::vector<T>& buf) {
    return hexdump(buf.data(), buf.size());
}

std::string hexdump(const PrfBlock& buf) {
    return hexdump(buf.data(), buf.size());
}

std::vector<char> decode(StringData sd) {
    auto s = hexblob::decode(sd);
    return std::vector<char>(s.data(), s.data() + s.length());
}

PrfBlock blockToArray(std::string block) {
    PrfBlock data;
    ASSERT_EQ(block.size(), data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = block[i];
    }
    return data;
}

PrfBlock decodePrf(StringData sd) {
    auto s = hexblob::decode(sd);
    return blockToArray(s);
}

std::basic_ostream<char>& operator<<(std::basic_ostream<char>& os, const std::vector<char>& right) {
    return os << hexdump(right);
}

std::basic_ostream<char>& operator<<(std::basic_ostream<char>& os,
                                     const std::vector<uint8_t>& right) {
    return os << hexdump(right);
}

template <FLETokenType tt>
std::basic_ostream<char>& operator<<(std::basic_ostream<char>& os, const FLEToken<tt>& right) {
    return os << "{" << static_cast<int>(right.type) << "," << hexdump(right.data) << "}";
}

FLEIndexKey indexKey(KeyMaterial{0x6e, 0xda, 0x88, 0xc8, 0x49, 0x6e, 0xc9, 0x90, 0xf5, 0xd5, 0x51,
                                 0x8d, 0xd2, 0xad, 0x6f, 0x3d, 0x9c, 0x33, 0xb6, 0x05, 0x59, 0x04,
                                 0xb1, 0x20, 0xf1, 0x2d, 0xe8, 0x29, 0x11, 0xfb, 0xd9, 0x33});

FLEUserKey userKey(KeyMaterial{0x1b, 0xd4, 0x32, 0xd4, 0xce, 0x54, 0x7d, 0xd7, 0xeb, 0xfb, 0x30,
                               0x9a, 0xea, 0xd6, 0xc6, 0x95, 0xfe, 0x53, 0xff, 0xe9, 0xc4, 0xb1,
                               0xc4, 0xf0, 0x6f, 0x36, 0x3c, 0xf0, 0x7b, 0x00, 0x28, 0xaf});

constexpr auto kIndexKeyId = "12345678-1234-9876-1234-123456789012"_sd;
constexpr auto kUserKeyId = "ABCDEFAB-1234-9876-1234-123456789012"_sd;
static UUID indexKeyId = uassertStatusOK(UUID::parse(kIndexKeyId.toString()));
static UUID userKeyId = uassertStatusOK(UUID::parse(kUserKeyId.toString()));

std::vector<char> testValue = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19};
std::vector<char> testValue2 = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29};

class TestKeyVault : public FLEKeyVault {
public:
    KeyMaterial getKey(UUID uuid) override;
};

KeyMaterial TestKeyVault::getKey(UUID uuid) {
    if (uuid == indexKeyId) {
        return indexKey.data;
    } else if (uuid == userKeyId) {
        return userKey.data;
    } else {
        FAIL("not implemented");
        return KeyMaterial{};
    }
}

TEST(FLETokens, TestVectors) {


    // Level 1
    auto collectionToken = FLELevel1TokenGenerator::generateCollectionsLevel1Token(indexKey);

    ASSERT_EQUALS(CollectionsLevel1Token(decodePrf(
                      "ff2103ff205a36f39704f643c270c129919f008c391d9589a6d2c86a7429d0d3"_sd)),
                  collectionToken);

    ASSERT_EQUALS(ServerDataEncryptionLevel1Token(decodePrf(
                      "d915ccc1eb81687fb5fc5b799f48c99fbe17e7a011a46a48901b9ae3d790656b"_sd)),
                  FLELevel1TokenGenerator::generateServerDataEncryptionLevel1Token(indexKey));

    // Level 2
    auto edcToken = FLECollectionTokenGenerator::generateEDCToken(collectionToken);
    ASSERT_EQUALS(
        EDCToken(decodePrf("167d2d2ff8e4144df37ff759db593fde0ecc7d9636f96d62dacad672eccad349"_sd)),

        edcToken);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(collectionToken);
    ASSERT_EQUALS(
        ESCToken(decodePrf("bfd480f1658f49f48985734737bc07d0bc36b88210277605c55ff3c9c3ef50b0"_sd)),
        escToken);
    auto eccToken = FLECollectionTokenGenerator::generateECCToken(collectionToken);
    ASSERT_EQUALS(
        ECCToken(decodePrf("9d34f9c182d75a5a3347c2f903e3e647105c651d52cf9555c9420ba07ddd3aa2"_sd)),
        eccToken);
    ASSERT_EQUALS(
        ECOCToken(decodePrf("e354e3b05e81e08b970ca061cb365163fd33dec2f982ddf9440e742ed288a8f8"_sd)),
        FLECollectionTokenGenerator::generateECOCToken(collectionToken));


    // Level 3
    std::vector<uint8_t> sampleValue = {0xc0, 0x7c, 0x0d, 0xf5, 0x12, 0x57, 0x94, 0x8e,
                                        0x1a, 0x0f, 0xc7, 0x0d, 0xd4, 0x56, 0x8e, 0x3a,
                                        0xf9, 0x9b, 0x23, 0xb3, 0x43, 0x4c, 0x98, 0x58,
                                        0x23, 0x7c, 0xa7, 0xdb, 0x62, 0xdb, 0x97, 0x66};

    auto edcDataToken =
        FLEDerivedFromDataTokenGenerator::generateEDCDerivedFromDataToken(edcToken, sampleValue);
    ASSERT_EQUALS(EDCDerivedFromDataToken(decodePrf(
                      "53eaa4c23a3ff65e6b7c7dbc4b1389cf0a6151b1ede5383a0673ff9c67855ff9"_sd)),
                  edcDataToken);

    auto escDataToken =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, sampleValue);
    ASSERT_EQUALS(ESCDerivedFromDataToken(decodePrf(
                      "acb3fab332131bbeaf112814f29ae0f2b10e97dc94b62db56c594661248e7467"_sd)),
                  escDataToken);

    auto eccDataToken =
        FLEDerivedFromDataTokenGenerator::generateECCDerivedFromDataToken(eccToken, sampleValue);
    ASSERT_EQUALS(ECCDerivedFromDataToken(decodePrf(
                      "826cfd35c35dcc7d4fbe13f33a3520749853bd1ea4c47919482252fba3a70cec"_sd)),
                  eccDataToken);

    // Level 4
    FLECounter counter = 1234567890;

    auto edcDataCounterToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateEDCDerivedFromDataTokenAndContentionFactorToken(edcDataToken, counter);
    ASSERT_EQUALS(EDCDerivedFromDataTokenAndContentionFactorToken(decodePrf(
                      "70fb9a3f760996f2f1438c5bf2a4d52bcba01b0badc3596276f49ffb2f0b136e"_sd)),
                  edcDataCounterToken);


    auto escDataCounterToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateESCDerivedFromDataTokenAndContentionFactorToken(escDataToken, counter);
    ASSERT_EQUALS(ESCDerivedFromDataTokenAndContentionFactorToken(decodePrf(
                      "7076c7b05fb4be4fe585eed930b852a6d088a0c55f3c96b50069e8a26ebfb347"_sd)),
                  escDataCounterToken);


    auto eccDataCounterToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateECCDerivedFromDataTokenAndContentionFactorToken(eccDataToken, counter);
    ASSERT_EQUALS(ECCDerivedFromDataTokenAndContentionFactorToken(decodePrf(
                      "6c6a349956c19f9c5e638e612011a71fbb71921edb540310c17cd0208b7f548b"_sd)),
                  eccDataCounterToken);


    // Level 5
    auto edcTwiceToken =
        FLETwiceDerivedTokenGenerator::generateEDCTwiceDerivedToken(edcDataCounterToken);
    ASSERT_EQUALS(EDCTwiceDerivedToken(decodePrf(
                      "3643fd370e2719c03234cdeec787dfdc7d8fceecafa8a992e3c1f9d4d53449fe"_sd)),
                  edcTwiceToken);

    auto escTwiceTagToken =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escDataCounterToken);
    ASSERT_EQUALS(ESCTwiceDerivedTagToken(decodePrf(
                      "c73bc4ff5e70222c653140b2b4998b4d62db973f20f116f66ff811a9a907a78f"_sd)),
                  escTwiceTagToken);
    auto escTwiceValueToken =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(escDataCounterToken);
    ASSERT_EQUALS(ESCTwiceDerivedValueToken(decodePrf(
                      "34150c6f5ab56dc39ddb935accb7f53e5276322fa937650b76a4dda9723d6fba"_sd)),
                  escTwiceValueToken);


    auto eccTwiceTagToken =
        FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedTagToken(eccDataCounterToken);
    ASSERT_EQUALS(ECCTwiceDerivedTagToken(decodePrf(
                      "0bc36f73062f5182c2403bffd155ec06eccfde0df8de5facaca4cc1cb320a385"_sd)),
                  eccTwiceTagToken);
    auto eccTwiceValueToken =
        FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedValueToken(eccDataCounterToken);
    ASSERT_EQUALS(ECCTwiceDerivedValueToken(decodePrf(
                      "2d7e08d58afa9f5ad215636e566d38584cbb48467d1bc9ff376eeca01fbfda6f"_sd)),
                  eccTwiceValueToken);
}


TEST(FLE_ESC, RoundTrip) {

    ConstDataRange value(testValue);

    auto c1 = FLELevel1TokenGenerator::generateCollectionsLevel1Token(indexKey);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(c1);

    ESCDerivedFromDataToken escDatakey =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, value);

    ESCDerivedFromDataTokenAndContentionFactorToken escDataCounterkey =
        FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
            generateESCDerivedFromDataTokenAndContentionFactorToken(escDatakey, 0);

    auto escTwiceTag =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escDataCounterkey);
    auto escTwiceValue =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(escDataCounterkey);


    {
        BSONObj doc =
            ESCCollection::generateNullDocument(escTwiceTag, escTwiceValue, 123, 123456789);
        auto swDoc = ESCCollection::decryptNullDocument(escTwiceValue, doc);
        ASSERT_OK(swDoc.getStatus());
        ASSERT_EQ(swDoc.getValue().position, 123);
        ASSERT_EQ(swDoc.getValue().count, 123456789);
    }


    {
        BSONObj doc =
            ESCCollection::generateInsertDocument(escTwiceTag, escTwiceValue, 123, 123456789);
        auto swDoc = ESCCollection::decryptDocument(escTwiceValue, doc);
        ASSERT_OK(swDoc.getStatus());
        ASSERT_EQ(swDoc.getValue().compactionPlaceholder, false);
        ASSERT_EQ(swDoc.getValue().position, 0);
        ASSERT_EQ(swDoc.getValue().count, 123456789);
    }

    {
        BSONObj doc = ESCCollection::generatePositionalDocument(
            escTwiceTag, escTwiceValue, 123, 456789, 123456789);
        auto swDoc = ESCCollection::decryptDocument(escTwiceValue, doc);
        ASSERT_OK(swDoc.getStatus());
        ASSERT_EQ(swDoc.getValue().compactionPlaceholder, false);
        ASSERT_EQ(swDoc.getValue().position, 456789);
        ASSERT_EQ(swDoc.getValue().count, 123456789);
    }

    {
        BSONObj doc =
            ESCCollection::generateCompactionPlaceholderDocument(escTwiceTag, escTwiceValue, 123);
        auto swDoc = ESCCollection::decryptDocument(escTwiceValue, doc);
        ASSERT_OK(swDoc.getStatus());
        ASSERT_EQ(swDoc.getValue().compactionPlaceholder, true);
        ASSERT_EQ(swDoc.getValue().position, std::numeric_limits<uint64_t>::max());
        ASSERT_EQ(swDoc.getValue().count, 0);
    }
}

TEST(FLE_ECC, RoundTrip) {

    ConstDataRange value(testValue);

    auto c1 = FLELevel1TokenGenerator::generateCollectionsLevel1Token(indexKey);
    auto token = FLECollectionTokenGenerator::generateECCToken(c1);

    ECCDerivedFromDataToken datakey =
        FLEDerivedFromDataTokenGenerator::generateECCDerivedFromDataToken(token, value);

    ECCDerivedFromDataTokenAndContentionFactorToken dataCounterkey =
        FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
            generateECCDerivedFromDataTokenAndContentionFactorToken(datakey, 0);

    auto twiceTag = FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedTagToken(dataCounterkey);
    auto twiceValue =
        FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedValueToken(dataCounterkey);


    {
        BSONObj doc = ECCCollection::generateNullDocument(twiceTag, twiceValue, 123456789);
        auto swDoc = ECCCollection::decryptNullDocument(twiceValue, doc);
        ASSERT_OK(swDoc.getStatus());
        ASSERT_EQ(swDoc.getValue().position, 123456789);
    }


    {
        BSONObj doc = ECCCollection::generateDocument(twiceTag, twiceValue, 123, 123456789);
        auto swDoc = ECCCollection::decryptDocument(twiceValue, doc);
        ASSERT_OK(swDoc.getStatus());
        ASSERT(swDoc.getValue().valueType == ECCValueType::kNormal);
        ASSERT_EQ(swDoc.getValue().start, 123456789);
        ASSERT_EQ(swDoc.getValue().end, 123456789);
    }

    {
        BSONObj doc =
            ECCCollection::generateDocument(twiceTag, twiceValue, 123, 123456789, 983456789);
        auto swDoc = ECCCollection::decryptDocument(twiceValue, doc);
        ASSERT_OK(swDoc.getStatus());
        ASSERT(swDoc.getValue().valueType == ECCValueType::kNormal);
        ASSERT_EQ(swDoc.getValue().start, 123456789);
        ASSERT_EQ(swDoc.getValue().end, 983456789);
    }

    {
        BSONObj doc = ECCCollection::generateCompactionDocument(twiceTag, twiceValue, 123456789);
        auto swDoc = ECCCollection::decryptDocument(twiceValue, doc);
        ASSERT_OK(swDoc.getStatus());
        ASSERT(swDoc.getValue().valueType == ECCValueType::kCompactionPlaceholder);
    }
}

class TestDocumentCollection : public FLEStateCollectionReader {
public:
    void insert(BSONObj& obj) {
        dassert(obj.firstElement().fieldNameStringData() == "_id"_sd);
        _docs.push_back(obj);
        // shuffle?
        // std::sort(_docs.begin(), _docs.end());
    }

    BSONObj getById(PrfBlock id) override {
        for (const auto& doc : _docs) {
            auto el = doc.firstElement();
            int len;
            auto p = el.binData(len);
            ASSERT_EQ(len, sizeof(PrfBlock));

            if (memcmp(p, id.data(), sizeof(PrfBlock)) == 0) {
                return doc;
            }
        }

        return BSONObj();
    }

    uint64_t getDocumentCount() override {
        return _docs.size();
    }

private:
    std::vector<BSONObj> _docs;
};

// Test Empty Collection
TEST(FLE_ESC, EmuBinary_Empty) {

    TestDocumentCollection coll;
    ConstDataRange value(testValue);

    auto c1 = FLELevel1TokenGenerator::generateCollectionsLevel1Token(indexKey);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(c1);

    ESCDerivedFromDataToken escDatakey =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, value);

    auto escDerivedToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateESCDerivedFromDataTokenAndContentionFactorToken(escDatakey, 0);

    auto escTwiceTag =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escDerivedToken);
    auto escTwiceValue =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(escDerivedToken);


    auto i = ESCCollection::emuBinary(&coll, escTwiceTag, escTwiceValue);

    ASSERT_TRUE(i.has_value());
    ASSERT_EQ(i.value(), 0);
}

// Test one new field in esc
TEST(FLE_ESC, EmuBinary) {

    TestDocumentCollection coll;
    ConstDataRange value(testValue);

    auto c1 = FLELevel1TokenGenerator::generateCollectionsLevel1Token(indexKey);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(c1);

    ESCDerivedFromDataToken escDatakey =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, value);

    auto escDerivedToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateESCDerivedFromDataTokenAndContentionFactorToken(escDatakey, 0);

    auto escTwiceTag =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escDerivedToken);
    auto escTwiceValue =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(escDerivedToken);

    for (int j = 1; j <= 5; j++) {
        BSONObj doc = ESCCollection::generateInsertDocument(escTwiceTag, escTwiceValue, j, j);
        coll.insert(doc);
    }

    auto i = ESCCollection::emuBinary(&coll, escTwiceTag, escTwiceValue);

    ASSERT_TRUE(i.has_value());
    ASSERT_EQ(i.value(), 5);
}


// Test two new fields in esc
TEST(FLE_ESC, EmuBinary2) {

    TestDocumentCollection coll;
    ConstDataRange value(testValue);

    auto c1 = FLELevel1TokenGenerator::generateCollectionsLevel1Token(indexKey);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(c1);


    ESCDerivedFromDataToken escDatakey2 =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, testValue2);

    auto escDerivedToken2 = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateESCDerivedFromDataTokenAndContentionFactorToken(escDatakey2, 0);

    auto escTwiceTag2 =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escDerivedToken2);
    auto escTwiceValue2 =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(escDerivedToken2);

    for (int j = 1; j <= 5; j++) {
        BSONObj doc = ESCCollection::generateInsertDocument(escTwiceTag2, escTwiceValue2, j, j);
        coll.insert(doc);
    }

    ESCDerivedFromDataToken escDatakey =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, value);

    auto escDerivedToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateESCDerivedFromDataTokenAndContentionFactorToken(escDatakey, 0);

    auto escTwiceTag =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escDerivedToken);
    auto escTwiceValue =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(escDerivedToken);


    for (int j = 1; j <= 13; j++) {
        BSONObj doc = ESCCollection::generateInsertDocument(escTwiceTag, escTwiceValue, j, j);
        coll.insert(doc);
    }

    auto i = ESCCollection::emuBinary(&coll, escTwiceTag, escTwiceValue);

    ASSERT_TRUE(i.has_value());
    ASSERT_EQ(i.value(), 13);

    i = ESCCollection::emuBinary(&coll, escTwiceTag2, escTwiceValue2);

    ASSERT_TRUE(i.has_value());
    ASSERT_EQ(i.value(), 5);
}

// Test Emulated Binary with null record
TEST(FLE_ESC, EmuBinary_NullRecord) {

    TestDocumentCollection coll;
    ConstDataRange value(testValue);

    auto c1 = FLELevel1TokenGenerator::generateCollectionsLevel1Token(indexKey);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(c1);

    ESCDerivedFromDataToken escDatakey =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, value);

    auto escDerivedToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateESCDerivedFromDataTokenAndContentionFactorToken(escDatakey, 0);

    auto escTwiceTag =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escDerivedToken);
    auto escTwiceValue =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(escDerivedToken);

    BSONObj doc = ESCCollection::generateNullDocument(escTwiceTag, escTwiceValue, 7, 7);
    coll.insert(doc);

    auto i = ESCCollection::emuBinary(&coll, escTwiceTag, escTwiceValue);

    ASSERT_FALSE(i.has_value());
}


std::vector<char> generatePlaceholder(BSONElement value) {
    FLE2EncryptionPlaceholder ep;

    ep.setAlgorithm(mongo::Fle2AlgorithmInt::kEquality);
    ep.setUserKeyId(userKeyId);
    ep.setIndexKeyId(indexKeyId);
    ep.setValue(value);
    ep.setType(mongo::Fle2PlaceholderType::kInsert);
    ep.setMaxContentionCounter(0);

    BSONObj obj = ep.toBSON();

    std::vector<char> v;
    v.resize(obj.objsize() + 1);
    v[0] = static_cast<uint8_t>(EncryptedBinDataType::kFLE2Placeholder);
    std::copy(obj.objdata(), obj.objdata() + obj.objsize(), v.begin() + 1);
    return v;
}

BSONObj encryptDocument(BSONObj obj, FLEKeyVault* keyVault) {
    auto result = FLEClientCrypto::generateInsertOrUpdateFromPlaceholders(obj, keyVault);

    // Start Server Side
    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);

    // TODO set count based on EmuBinary
    for (auto& payload : serverPayload) {
        payload.count = 1;
    }

    // Finalize document for insert
    auto finalDoc = EDCServerCollection::finalizeForInsert(result, serverPayload);
    ASSERT_EQ(finalDoc["__safeContent__"].type(), Array);
    return finalDoc;
}

void roundTripTest(BSONObj doc, BSONType type) {

    auto element = doc.firstElement();
    ASSERT_EQ(element.type(), type);


    TestKeyVault keyVault;

    auto inputDoc = BSON("plainText"
                         << "sample"
                         << "encrypted" << element);

    auto buf = generatePlaceholder(element);
    BSONObjBuilder builder;
    builder.append("plainText", "sample");
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());

    auto finalDoc = encryptDocument(builder.obj(), &keyVault);

    ASSERT_EQ(finalDoc["plainText"].type(), String);
    ASSERT_EQ(finalDoc["encrypted"].type(), BinData);
    ASSERT_TRUE(finalDoc["encrypted"].isBinData(BinDataType::Encrypt));

    // Decrypt document
    auto decryptedDoc = FLEClientCrypto::decryptDocument(finalDoc, &keyVault);

    // Remove this so the round-trip is clean
    decryptedDoc = decryptedDoc.removeField("__safeContent__");

    ASSERT_BSONOBJ_EQ(inputDoc, decryptedDoc);
}

TEST(FLE_EDC, Allowed_Types) {
    roundTripTest(BSON("sample"
                       << "value123"),
                  String);
    roundTripTest(BSON("sample" << BSONBinData(
                           testValue.data(), testValue.size(), BinDataType::BinDataGeneral)),
                  BinData);
    roundTripTest(BSON("sample" << OID()), jstOID);


    roundTripTest(BSON("sample" << false), Bool);
    roundTripTest(BSON("sample" << true), Bool);
    roundTripTest(BSON("sample" << Date_t()), Date);
    roundTripTest(BSON("sample" << BSONRegEx("value1", "value2")), RegEx);
    roundTripTest(BSON("sample" << 123456), NumberInt);
    roundTripTest(BSON("sample" << Timestamp()), bsonTimestamp);
    roundTripTest(BSON("sample" << 12345678901234567LL), NumberLong);
    roundTripTest(BSON("sample" << BSONCode("value")), Code);
}

void illegalBSONType(BSONObj doc, BSONType type) {
    auto element = doc.firstElement();
    if (isValidBSONType(type)) {
        ASSERT_EQ(element.type(), type);
    }

    TestKeyVault keyVault;

    auto buf = generatePlaceholder(element);
    BSONObjBuilder builder;
    builder.append("plainText", "sample");
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());
    BSONObj obj = builder.obj();

    ASSERT_THROWS_CODE(FLEClientCrypto::generateInsertOrUpdateFromPlaceholders(obj, &keyVault),
                       DBException,
                       6338602);
}

TEST(FLE_EDC, Disallowed_Types) {
    illegalBSONType(BSON("sample" << 123.456), NumberDouble);
    illegalBSONType(BSON("sample" << Decimal128()), NumberDecimal);

    illegalBSONType(BSON("sample" << MINKEY), MinKey);

    illegalBSONType(BSON("sample" << BSON("nested"
                                          << "value")),
                    Object);
    illegalBSONType(BSON("sample" << BSON_ARRAY(1 << 23)), Array);

    illegalBSONType(BSON("sample" << BSONUndefined), Undefined);
    illegalBSONType(BSON("sample" << BSONNULL), jstNULL);
    illegalBSONType(BSON("sample" << BSONDBRef("value1", OID())), DBRef);
    illegalBSONType(BSON("sample" << BSONSymbol("value")), Symbol);
    illegalBSONType(BSON("sample" << BSONCodeWScope("value",
                                                    BSON("code"
                                                         << "something"))),
                    CodeWScope);

    illegalBSONType(BSON("sample" << MAXKEY), MaxKey);


    uint8_t fakeBSONType = 42;
    ASSERT_FALSE(isValidBSONType(fakeBSONType));
    illegalBSONType(BSON("sample" << 123.456), static_cast<BSONType>(fakeBSONType));
}


template <typename T>
T parseFromCDR(ConstDataRange cdr) {
    ConstDataRangeCursor cdc(cdr);
    auto swObj = cdc.readAndAdvanceNoThrow<Validated<BSONObj>>();

    uassertStatusOK(swObj);

    BSONObj obj = swObj.getValue();

    IDLParserErrorContext ctx("root");
    return T::parse(ctx, obj);
}

template <typename T>
std::vector<uint8_t> toEncryptedVector(EncryptedBinDataType dt, T t) {
    BSONObj obj = t.toBSON();

    std::vector<uint8_t> buf(obj.objsize() + 1);
    buf[0] = static_cast<uint8_t>(dt);

    std::copy(obj.objdata(), obj.objdata() + obj.objsize(), buf.data() + 1);

    return buf;
}

template <typename T>
void toEncryptedBinData(StringData field, EncryptedBinDataType dt, T t, BSONObjBuilder* builder) {
    auto buf = toEncryptedVector(dt, t);

    builder->appendBinData(field, buf.size(), BinDataType::Encrypt, buf.data());
}


std::pair<EncryptedBinDataType, ConstDataRange> fromEncryptedConstDataRange(ConstDataRange cdr) {
    ConstDataRangeCursor cdrc(cdr);

    uint8_t subTypeByte = cdrc.readAndAdvance<uint8_t>();

    auto subType = EncryptedBinDataType_parse(IDLParserErrorContext("subtype"), subTypeByte);
    return {subType, cdrc};
}


BSONObj transformBSON(
    const BSONObj& object,
    const std::function<void(ConstDataRange, BSONObjBuilder*, StringData)>& doTransform) {
    struct IteratorState {
        BSONObjIterator iter;
        BSONObjBuilder builder;
    };

    std::stack<IteratorState> frameStack;

    const ScopeGuard frameStackGuard([&] {
        while (!frameStack.empty()) {
            frameStack.pop();
        }
    });

    frameStack.push({BSONObjIterator(object), BSONObjBuilder()});

    while (frameStack.size() > 1 || frameStack.top().iter.more()) {
        ASSERT(frameStack.size() < BSONDepth::kDefaultMaxAllowableDepth);
        auto& [iterator, builder] = frameStack.top();
        if (iterator.more()) {
            BSONElement elem = iterator.next();
            if (elem.type() == BSONType::Object) {
                frameStack.push({BSONObjIterator(elem.Obj()),
                                 BSONObjBuilder(builder.subobjStart(elem.fieldNameStringData()))});
            } else if (elem.type() == BSONType::Array) {
                frameStack.push(
                    {BSONObjIterator(elem.Obj()),
                     BSONObjBuilder(builder.subarrayStart(elem.fieldNameStringData()))});
            } else if (elem.isBinData(BinDataType::Encrypt)) {
                int len;
                const char* data(elem.binData(len));
                ConstDataRange cdr(data, len);
                doTransform(cdr, &builder, elem.fieldNameStringData());
            } else {
                builder.append(elem);
            }
        } else {
            frameStack.pop();
        }
    }
    invariant(frameStack.size() == 1);
    return frameStack.top().builder.obj();
}

void disallowedEqualityPayloadType(BSONType type) {
    auto doc = BSON("sample" << 123456);
    auto element = doc.firstElement();

    TestKeyVault keyVault;


    auto inputDoc = BSON("plainText"
                         << "sample"
                         << "encrypted" << element);

    auto buf = generatePlaceholder(element);
    BSONObjBuilder builder;
    builder.append("plainText", "sample");
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());
    BSONObj obj = builder.obj();

    auto result = FLEClientCrypto::generateInsertOrUpdateFromPlaceholders(obj, &keyVault);

    // Since FLEClientCrypto::generateInsertOrUpdateFromPlaceholders validates the type is correct,
    // we send an allowed type and then change the type to something that is not allowed
    result = transformBSON(
        result,
        [type](ConstDataRange cdr, BSONObjBuilder* builder, StringData fieldNameToSerialize) {
            auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(cdr);


            auto iup = parseFromCDR<FLE2InsertUpdatePayload>(subCdr);

            iup.setType(type);
            toEncryptedBinData(
                fieldNameToSerialize, EncryptedBinDataType::kFLE2InsertUpdatePayload, iup, builder);
        });


    // Start Server Side
    ASSERT_THROWS_CODE(EDCServerCollection::getEncryptedFieldInfo(result), DBException, 6373504);
}

TEST(FLE_EDC, Disallowed_Types_FLE2InsertUpdatePayload) {
    disallowedEqualityPayloadType(NumberDouble);
    disallowedEqualityPayloadType(NumberDecimal);

    disallowedEqualityPayloadType(MinKey);

    disallowedEqualityPayloadType(Object);
    disallowedEqualityPayloadType(Array);

    disallowedEqualityPayloadType(Undefined);
    disallowedEqualityPayloadType(jstNULL);
    disallowedEqualityPayloadType(DBRef);
    disallowedEqualityPayloadType(Symbol);
    disallowedEqualityPayloadType(CodeWScope);

    disallowedEqualityPayloadType(MaxKey);

    uint8_t fakeBSONType = 42;
    ASSERT_FALSE(isValidBSONType(fakeBSONType));
    disallowedEqualityPayloadType(static_cast<BSONType>(fakeBSONType));
}


TEST(FLE_EDC, ServerSide_Payloads) {
    auto doc = BSON("sample" << 123456);
    auto element = doc.firstElement();

    auto value = ConstDataRange(element.value(), element.value() + element.valuesize());

    auto collectionToken = FLELevel1TokenGenerator::generateCollectionsLevel1Token(indexKey);
    auto serverEncryptToken =
        FLELevel1TokenGenerator::generateServerDataEncryptionLevel1Token(indexKey);
    auto edcToken = FLECollectionTokenGenerator::generateEDCToken(collectionToken);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(collectionToken);
    auto eccToken = FLECollectionTokenGenerator::generateECCToken(collectionToken);
    auto ecocToken = FLECollectionTokenGenerator::generateECOCToken(collectionToken);

    FLECounter counter = 0;


    EDCDerivedFromDataToken edcDatakey =
        FLEDerivedFromDataTokenGenerator::generateEDCDerivedFromDataToken(edcToken, value);
    ESCDerivedFromDataToken escDatakey =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, value);
    ECCDerivedFromDataToken eccDatakey =
        FLEDerivedFromDataTokenGenerator::generateECCDerivedFromDataToken(eccToken, value);


    ESCDerivedFromDataTokenAndContentionFactorToken escDataCounterkey =
        FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
            generateESCDerivedFromDataTokenAndContentionFactorToken(escDatakey, counter);
    ECCDerivedFromDataTokenAndContentionFactorToken eccDataCounterkey =
        FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
            generateECCDerivedFromDataTokenAndContentionFactorToken(eccDatakey, counter);

    FLE2InsertUpdatePayload iupayload;


    iupayload.setEdcDerivedToken(edcDatakey.toCDR());
    iupayload.setEscDerivedToken(escDatakey.toCDR());
    iupayload.setEccDerivedToken(eccDatakey.toCDR());
    iupayload.setServerEncryptionToken(serverEncryptToken.toCDR());

    auto swEncryptedTokens =
        EncryptedStateCollectionTokens(escDataCounterkey, eccDataCounterkey).serialize(ecocToken);
    uassertStatusOK(swEncryptedTokens);
    iupayload.setEncryptedTokens(swEncryptedTokens.getValue());

    std::vector<uint8_t> cipherText = {0x1, 0x2, 0x3, 0x4, 0x5};
    iupayload.setValue(cipherText);
    iupayload.setType(element.type());

    FLE2IndexedEqualityEncryptedValue serverPayload(iupayload, 123456);

    auto swBuf = serverPayload.serialize(serverEncryptToken);
    ASSERT_OK(swBuf.getStatus());

    auto swServerPayload =
        FLE2IndexedEqualityEncryptedValue::decryptAndParse(serverEncryptToken, swBuf.getValue());

    ASSERT_OK(swServerPayload.getStatus());
    auto sp = swServerPayload.getValue();
    ASSERT_EQ(sp.edc, serverPayload.edc);
    ASSERT_EQ(sp.esc, serverPayload.esc);
    ASSERT_EQ(sp.ecc, serverPayload.ecc);
    ASSERT_EQ(sp.count, serverPayload.count);
    ASSERT(sp.clientEncryptedValue == serverPayload.clientEncryptedValue);
    ASSERT(cipherText == serverPayload.clientEncryptedValue);
}

TEST(FLE_EDC, DuplicateSafeContent_CompatibleType) {

    TestKeyVault keyVault;

    auto doc = BSON("value"
                    << "123456");
    auto element = doc.firstElement();
    auto inputDoc = BSON("__safeContent__" << BSON_ARRAY(1 << 2 << 4) << "encrypted" << element);

    auto buf = generatePlaceholder(element);
    BSONObjBuilder builder;
    builder.append("__safeContent__", BSON_ARRAY(1 << 2 << 4));
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());

    auto finalDoc = encryptDocument(builder.obj(), &keyVault);

    ASSERT_EQ(finalDoc["__safeContent__"].type(), Array);
    ASSERT_EQ(finalDoc["encrypted"].type(), BinData);
    ASSERT_TRUE(finalDoc["encrypted"].isBinData(BinDataType::Encrypt));

    // Decrypt document
    auto decryptedDoc = FLEClientCrypto::decryptDocument(finalDoc, &keyVault);

    std::cout << "Final Doc: " << decryptedDoc << std::endl;

    auto elements = finalDoc["__safeContent__"].Array();
    ASSERT_EQ(elements.size(), 4);
    ASSERT_EQ(elements[0].safeNumberInt(), 1);
    ASSERT_EQ(elements[1].safeNumberInt(), 2);
    ASSERT_EQ(elements[2].safeNumberInt(), 4);
    ASSERT(elements[3].type() == BinData);
}


TEST(FLE_EDC, DuplicateSafeContent_IncompatibleType) {

    TestKeyVault keyVault;

    auto doc = BSON("value"
                    << "123456");
    auto element = doc.firstElement();

    auto buf = generatePlaceholder(element);
    BSONObjBuilder builder;
    builder.append("__safeContent__", 123456);
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());

    ASSERT_THROWS_CODE(encryptDocument(builder.obj(), &keyVault), DBException, 6373510);
}

template <typename T, typename Func>
bool vectorContains(const std::vector<T>& vec, Func func) {
    return std::find_if(vec.begin(), vec.end(), func) != vec.end();
}

EncryptedFieldConfig getTestEncryptedFieldConfig() {

    constexpr auto schema = R"({
    "escCollection": "esc",
    "eccCollection": "ecc",
    "ecocCollection": "ecoc",
    "fields": [
        {
            "keyId":
                            {
                                "$uuid": "12345678-1234-9876-1234-123456789012"
                            }
                        ,
            "path": "encrypted",
            "bsonType": "string",
            "queries": {"queryType": "equality"}

        },
        {
            "keyId":
                            {
                                "$uuid": "12345678-1234-9876-1234-123456789012"
                            }
                        ,
            "path": "nested.encrypted",
            "bsonType": "string",
            "queries": {"queryType": "equality"}

        },
        {
            "keyId":
                            {
                                "$uuid": "12345678-1234-9876-1234-123456789012"
                            }
                        ,
            "path": "nested.notindexed",
            "bsonType": "string"
        }
    ]
})";

    return EncryptedFieldConfig::parse(IDLParserErrorContext("root"), fromjson(schema));
}

TEST(EncryptionInformation, RoundTrip) {
    NamespaceString ns("test.test");

    EncryptedFieldConfig efc = getTestEncryptedFieldConfig();
    auto obj = EncryptionInformationHelpers::encryptionInformationSerialize(ns, efc);


    EncryptedFieldConfig efc2 = EncryptionInformationHelpers::getAndValidateSchema(
        ns, EncryptionInformation::parse(IDLParserErrorContext("foo"), obj));

    ASSERT_BSONOBJ_EQ(efc.toBSON(), efc2.toBSON());
}

TEST(EncryptionInformation, BadSchema) {
    EncryptionInformation ei;
    ei.setType(1);

    ei.setSchema(BSON("a"
                      << "b"));

    auto obj = ei.toBSON();

    NamespaceString ns("test.test");
    ASSERT_THROWS_CODE(EncryptionInformationHelpers::getAndValidateSchema(
                           ns, EncryptionInformation::parse(IDLParserErrorContext("foo"), obj)),
                       DBException,
                       6371205);
}

TEST(EncryptionInformation, MissingStateCollection) {
    NamespaceString ns("test.test");

    {
        EncryptedFieldConfig efc = getTestEncryptedFieldConfig();
        efc.setEscCollection(boost::none);
        auto obj = EncryptionInformationHelpers::encryptionInformationSerialize(ns, efc);
        ASSERT_THROWS_CODE(EncryptionInformationHelpers::getAndValidateSchema(
                               ns, EncryptionInformation::parse(IDLParserErrorContext("foo"), obj)),
                           DBException,
                           6371207);
    }
    {
        EncryptedFieldConfig efc = getTestEncryptedFieldConfig();
        efc.setEccCollection(boost::none);
        auto obj = EncryptionInformationHelpers::encryptionInformationSerialize(ns, efc);
        ASSERT_THROWS_CODE(EncryptionInformationHelpers::getAndValidateSchema(
                               ns, EncryptionInformation::parse(IDLParserErrorContext("foo"), obj)),
                           DBException,
                           6371206);
    }
    {
        EncryptedFieldConfig efc = getTestEncryptedFieldConfig();
        efc.setEcocCollection(boost::none);
        auto obj = EncryptionInformationHelpers::encryptionInformationSerialize(ns, efc);
        ASSERT_THROWS_CODE(EncryptionInformationHelpers::getAndValidateSchema(
                               ns, EncryptionInformation::parse(IDLParserErrorContext("foo"), obj)),
                           DBException,
                           6371208);
    }
}

TEST(IndexedFields, FetchTwoLevels) {
    TestKeyVault keyVault;

    auto doc = BSON("value"
                    << "123456");
    auto element = doc.firstElement();
    auto inputDoc = BSON("__safeContent__" << BSON_ARRAY(1 << 2 << 4) << "encrypted" << element);

    auto buf = generatePlaceholder(element);
    BSONObjBuilder builder;
    builder.append("__safeContent__", BSON_ARRAY(1 << 2 << 4));
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());
    {
        BSONObjBuilder sub(builder.subobjStart("nested"));
        sub.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());
        {
            BSONObjBuilder sub2(sub.subobjStart("nested2"));
            sub2.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());
        }
    }

    auto obj = builder.obj();

    auto noIndexedFields = EDCServerCollection::getEncryptedIndexedFields(obj);

    ASSERT_EQ(noIndexedFields.size(), 0);

    auto finalDoc = encryptDocument(obj, &keyVault);

    auto indexedFields = EDCServerCollection::getEncryptedIndexedFields(finalDoc);

    ASSERT_EQ(indexedFields.size(), 3);


    ASSERT(vectorContains(indexedFields,
                          [](EDCIndexedFields i) { return i.fieldPathName == "encrypted"; }));
    ASSERT(vectorContains(
        indexedFields, [](EDCIndexedFields i) { return i.fieldPathName == "nested.encrypted"; }));
    ASSERT(vectorContains(indexedFields, [](EDCIndexedFields i) {
        return i.fieldPathName == "nested.nested2.encrypted";
    }));
}


}  // namespace mongo

#endif
