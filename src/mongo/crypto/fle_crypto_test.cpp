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

#include "mongo/db/operation_context.h"
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
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/db/matcher/schema/encrypt_schema_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/platform/decimal128.h"
#include "mongo/rpc/object_check.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/time_support.h"


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

constexpr auto kIndexKeyId = "12345678-1234-9876-1234-123456789012"_sd;
constexpr auto kIndexKey2Id = "12345678-1234-9876-1234-123456789013"_sd;
constexpr auto kIndexKey3Id = "12345678-1234-9876-1234-123456789014"_sd;
constexpr auto kUserKeyId = "ABCDEFAB-1234-9876-1234-123456789012"_sd;
static UUID indexKeyId = uassertStatusOK(UUID::parse(kIndexKeyId.toString()));
static UUID indexKey2Id = uassertStatusOK(UUID::parse(kIndexKey2Id.toString()));
static UUID indexKey3Id = uassertStatusOK(UUID::parse(kIndexKey3Id.toString()));
static UUID userKeyId = uassertStatusOK(UUID::parse(kUserKeyId.toString()));

std::vector<char> testValue = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19};
std::vector<char> testValue2 = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29};

const FLEIndexKey& getIndexKey() {
    static std::string indexVec = hexblob::decode(
        "7dbfebc619aa68a659f64b8e23ccd21644ac326cb74a26840c3d2420176c40ae088294d00ad6cae9684237b21b754cf503f085c25cd320bf035c3417416e1e6fe3d9219f79586582112740b2add88e1030d91926ae8afc13ee575cfb8bb965b7"_sd);
    static FLEIndexKey indexKey(KeyMaterial(indexVec.begin(), indexVec.end()));
    return indexKey;
}

const FLEIndexKey& getIndex2Key() {
    static std::string index2Vec = hexblob::decode(
        "1f65c3223d5653cdbd73c11a8f85587aafcbd5be7e4c308d357b2f01bbcf76a9802930e5f233923bbc3f5ebd0be1db9807f04aa870c896092180dd8b05816b8f7568ff762a1a4efd35bbc02826394eb30f36cd8e0c646ae2f43df420e50a19eb"_sd);
    static FLEIndexKey index2Key(KeyMaterial(index2Vec.begin(), index2Vec.end()));
    return index2Key;
}

const FLEIndexKey& getIndex3Key() {
    static std::string index3Vec = hexblob::decode(
        "1f65c3223d5653cdbd73c11a8f85587aafcbd5be7e4c308d357b2f01bbcf76a9802930e5f233923bbc3f5ebd0be1db9807f04aa870c896092180dd8b05816b8f7568ff762a1a4efd35bbc02826394eb30f36cd8e0c646ae2f43df420e50a19eb"_sd);
    static FLEIndexKey index3Key(KeyMaterial(index3Vec.begin(), index3Vec.end()));
    return index3Key;
}

const FLEUserKey& getUserKey() {
    static std::string userVec = hexblob::decode(
        "a7ddbc4c8be00d51f68d9d8e485f351c8edc8d2206b24d8e0e1816d005fbe520e489125047d647b0d8684bfbdbf09c304085ed086aba6c2b2b1677ccc91ced8847a733bf5e5682c84b3ee7969e4a5fe0e0c21e5e3ee190595a55f83147d8de2a"_sd);
    static FLEUserKey userKey(KeyMaterial(userVec.begin(), userVec.end()));
    return userKey;
}

class TestKeyVault : public FLEKeyVault {
public:
    KeyMaterial getKey(const UUID& uuid) override;
};

KeyMaterial TestKeyVault::getKey(const UUID& uuid) {
    if (uuid == indexKeyId) {
        return getIndexKey().data;
    } else if (uuid == indexKey2Id) {
        return getIndex2Key().data;
    } else if (uuid == indexKey3Id) {
        return getIndex3Key().data;
    } else if (uuid == userKeyId) {
        return getUserKey().data;
    } else {
        FAIL("not implemented");
        return KeyMaterial();
    }
}

// TODO (SERVER-63594) - regenerate test vectors and reimplement test
//
// TEST(FLETokens, TestVectors) {

//     // Level 1
//     auto collectionToken =
//     FLELevel1TokenGenerator::generateCollectionsLevel1Token(getIndexKey());

//     ASSERT_EQUALS(CollectionsLevel1Token(decodePrf(
//                       "ff2103ff205a36f39704f643c270c129919f008c391d9589a6d2c86a7429d0d3"_sd)),
//                   collectionToken);

//     ASSERT_EQUALS(ServerDataEncryptionLevel1Token(decodePrf(
//                       "d915ccc1eb81687fb5fc5b799f48c99fbe17e7a011a46a48901b9ae3d790656b"_sd)),
//                   FLELevel1TokenGenerator::generateServerDataEncryptionLevel1Token(getIndexKey()));

//     // Level 2
//     auto edcToken = FLECollectionTokenGenerator::generateEDCToken(collectionToken);
//     ASSERT_EQUALS(
//         EDCToken(decodePrf("167d2d2ff8e4144df37ff759db593fde0ecc7d9636f96d62dacad672eccad349"_sd)),

//         edcToken);
//     auto escToken = FLECollectionTokenGenerator::generateESCToken(collectionToken);
//     ASSERT_EQUALS(
//         ESCToken(decodePrf("bfd480f1658f49f48985734737bc07d0bc36b88210277605c55ff3c9c3ef50b0"_sd)),
//         escToken);
//     auto eccToken = FLECollectionTokenGenerator::generateECCToken(collectionToken);
//     ASSERT_EQUALS(
//         ECCToken(decodePrf("9d34f9c182d75a5a3347c2f903e3e647105c651d52cf9555c9420ba07ddd3aa2"_sd)),
//         eccToken);
//     ASSERT_EQUALS(
//         ECOCToken(decodePrf("e354e3b05e81e08b970ca061cb365163fd33dec2f982ddf9440e742ed288a8f8"_sd)),
//         FLECollectionTokenGenerator::generateECOCToken(collectionToken));


//     // Level 3
//     std::vector<uint8_t> sampleValue = {0xc0, 0x7c, 0x0d, 0xf5, 0x12, 0x57, 0x94, 0x8e,
//                                         0x1a, 0x0f, 0xc7, 0x0d, 0xd4, 0x56, 0x8e, 0x3a,
//                                         0xf9, 0x9b, 0x23, 0xb3, 0x43, 0x4c, 0x98, 0x58,
//                                         0x23, 0x7c, 0xa7, 0xdb, 0x62, 0xdb, 0x97, 0x66};

//     auto edcDataToken =
//         FLEDerivedFromDataTokenGenerator::generateEDCDerivedFromDataToken(edcToken, sampleValue);
//     ASSERT_EQUALS(EDCDerivedFromDataToken(decodePrf(
//                       "53eaa4c23a3ff65e6b7c7dbc4b1389cf0a6151b1ede5383a0673ff9c67855ff9"_sd)),
//                   edcDataToken);

//     auto escDataToken =
//         FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, sampleValue);
//     ASSERT_EQUALS(ESCDerivedFromDataToken(decodePrf(
//                       "acb3fab332131bbeaf112814f29ae0f2b10e97dc94b62db56c594661248e7467"_sd)),
//                   escDataToken);

//     auto eccDataToken =
//         FLEDerivedFromDataTokenGenerator::generateECCDerivedFromDataToken(eccToken, sampleValue);
//     ASSERT_EQUALS(ECCDerivedFromDataToken(decodePrf(
//                       "826cfd35c35dcc7d4fbe13f33a3520749853bd1ea4c47919482252fba3a70cec"_sd)),
//                   eccDataToken);

//     // Level 4
//     FLECounter counter = 1234567890;

//     auto edcDataCounterToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
//         generateEDCDerivedFromDataTokenAndContentionFactorToken(edcDataToken, counter);
//     ASSERT_EQUALS(EDCDerivedFromDataTokenAndContentionFactorToken(decodePrf(
//                       "70fb9a3f760996f2f1438c5bf2a4d52bcba01b0badc3596276f49ffb2f0b136e"_sd)),
//                   edcDataCounterToken);


//     auto escDataCounterToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
//         generateESCDerivedFromDataTokenAndContentionFactorToken(escDataToken, counter);
//     ASSERT_EQUALS(ESCDerivedFromDataTokenAndContentionFactorToken(decodePrf(
//                       "7076c7b05fb4be4fe585eed930b852a6d088a0c55f3c96b50069e8a26ebfb347"_sd)),
//                   escDataCounterToken);


//     auto eccDataCounterToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
//         generateECCDerivedFromDataTokenAndContentionFactorToken(eccDataToken, counter);
//     ASSERT_EQUALS(ECCDerivedFromDataTokenAndContentionFactorToken(decodePrf(
//                       "6c6a349956c19f9c5e638e612011a71fbb71921edb540310c17cd0208b7f548b"_sd)),
//                   eccDataCounterToken);


//     // Level 5
//     auto edcTwiceToken =
//         FLETwiceDerivedTokenGenerator::generateEDCTwiceDerivedToken(edcDataCounterToken);
//     ASSERT_EQUALS(EDCTwiceDerivedToken(decodePrf(
//                       "3643fd370e2719c03234cdeec787dfdc7d8fceecafa8a992e3c1f9d4d53449fe"_sd)),
//                   edcTwiceToken);

//     auto escTwiceTagToken =
//         FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escDataCounterToken);
//     ASSERT_EQUALS(ESCTwiceDerivedTagToken(decodePrf(
//                       "c73bc4ff5e70222c653140b2b4998b4d62db973f20f116f66ff811a9a907a78f"_sd)),
//                   escTwiceTagToken);
//     auto escTwiceValueToken =
//         FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(escDataCounterToken);
//     ASSERT_EQUALS(ESCTwiceDerivedValueToken(decodePrf(
//                       "34150c6f5ab56dc39ddb935accb7f53e5276322fa937650b76a4dda9723d6fba"_sd)),
//                   escTwiceValueToken);


//     auto eccTwiceTagToken =
//         FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedTagToken(eccDataCounterToken);
//     ASSERT_EQUALS(ECCTwiceDerivedTagToken(decodePrf(
//                       "0bc36f73062f5182c2403bffd155ec06eccfde0df8de5facaca4cc1cb320a385"_sd)),
//                   eccTwiceTagToken);
//     auto eccTwiceValueToken =
//         FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedValueToken(eccDataCounterToken);
//     ASSERT_EQUALS(ECCTwiceDerivedValueToken(decodePrf(
//                       "2d7e08d58afa9f5ad215636e566d38584cbb48467d1bc9ff376eeca01fbfda6f"_sd)),
//                   eccTwiceValueToken);
// }

TEST(FLE_ESC, RoundTrip) {
    TestKeyVault keyVault;

    ConstDataRange value(testValue);

    auto c1 = FLELevel1TokenGenerator::generateCollectionsLevel1Token(getIndexKey());
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
        BSONObj doc = ESCCollection::generateCompactionPlaceholderDocument(
            escTwiceTag, escTwiceValue, 123, 456789);
        auto swDoc = ESCCollection::decryptDocument(escTwiceValue, doc);
        ASSERT_OK(swDoc.getStatus());
        ASSERT_EQ(swDoc.getValue().compactionPlaceholder, true);
        ASSERT_EQ(swDoc.getValue().position, std::numeric_limits<uint64_t>::max());
        ASSERT_EQ(swDoc.getValue().count, 456789);
    }
}

TEST(FLE_ECC, RoundTrip) {
    TestKeyVault keyVault;

    ConstDataRange value(testValue);

    auto c1 = FLELevel1TokenGenerator::generateCollectionsLevel1Token(getIndexKey());
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

    BSONObj getById(PrfBlock id) const override {
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

    uint64_t getDocumentCount() const override {
        return _docs.size();
    }

private:
    std::vector<BSONObj> _docs;
};

// Test Empty Collection
TEST(FLE_ESC, EmuBinary_Empty) {
    TestKeyVault keyVault;

    TestDocumentCollection coll;
    ConstDataRange value(testValue);

    auto c1 = FLELevel1TokenGenerator::generateCollectionsLevel1Token(getIndexKey());
    auto escToken = FLECollectionTokenGenerator::generateESCToken(c1);

    ESCDerivedFromDataToken escDatakey =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, value);

    auto escDerivedToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateESCDerivedFromDataTokenAndContentionFactorToken(escDatakey, 0);

    auto escTwiceTag =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escDerivedToken);
    auto escTwiceValue =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(escDerivedToken);


    auto i = ESCCollection::emuBinary(coll, escTwiceTag, escTwiceValue);

    ASSERT_TRUE(i.has_value());
    ASSERT_EQ(i.value(), 0);
}

// Test one new field in esc
TEST(FLE_ESC, EmuBinary) {
    TestKeyVault keyVault;

    TestDocumentCollection coll;
    ConstDataRange value(testValue);

    auto c1 = FLELevel1TokenGenerator::generateCollectionsLevel1Token(getIndexKey());
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

    auto i = ESCCollection::emuBinary(coll, escTwiceTag, escTwiceValue);

    ASSERT_TRUE(i.has_value());
    ASSERT_EQ(i.value(), 5);
}


// Test two new fields in esc
TEST(FLE_ESC, EmuBinary2) {
    TestKeyVault keyVault;

    TestDocumentCollection coll;
    ConstDataRange value(testValue);

    auto c1 = FLELevel1TokenGenerator::generateCollectionsLevel1Token(getIndexKey());
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

    auto i = ESCCollection::emuBinary(coll, escTwiceTag, escTwiceValue);

    ASSERT_TRUE(i.has_value());
    ASSERT_EQ(i.value(), 13);

    i = ESCCollection::emuBinary(coll, escTwiceTag2, escTwiceValue2);

    ASSERT_TRUE(i.has_value());
    ASSERT_EQ(i.value(), 5);
}

// Test Emulated Binary with null record
TEST(FLE_ESC, EmuBinary_NullRecord) {
    TestKeyVault keyVault;

    TestDocumentCollection coll;
    ConstDataRange value(testValue);

    auto c1 = FLELevel1TokenGenerator::generateCollectionsLevel1Token(getIndexKey());
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

    auto i = ESCCollection::emuBinary(coll, escTwiceTag, escTwiceValue);

    ASSERT_FALSE(i.has_value());
}

enum class Operation { kFind, kInsert };

std::vector<char> generatePlaceholder(
    BSONElement value,
    Operation operation,
    mongo::Fle2AlgorithmInt algorithm = mongo::Fle2AlgorithmInt::kEquality,
    boost::optional<UUID> key = boost::none) {
    FLE2EncryptionPlaceholder ep;

    if (operation == Operation::kFind) {
        ep.setType(mongo::Fle2PlaceholderType::kFind);
    } else if (operation == Operation::kInsert) {
        ep.setType(mongo::Fle2PlaceholderType::kInsert);
    }

    ep.setAlgorithm(algorithm);
    ep.setUserKeyId(userKeyId);
    ep.setIndexKeyId(key.value_or(indexKeyId));
    ep.setValue(value);
    ep.setMaxContentionCounter(0);

    BSONObj obj = ep.toBSON();

    std::vector<char> v;
    v.resize(obj.objsize() + 1);
    v[0] = static_cast<uint8_t>(EncryptedBinDataType::kFLE2Placeholder);
    std::copy(obj.objdata(), obj.objdata() + obj.objsize(), v.begin() + 1);
    return v;
}

BSONObj encryptDocument(BSONObj obj,
                        FLEKeyVault* keyVault,
                        const EncryptedFieldConfig* efc = nullptr) {
    auto result = FLEClientCrypto::transformPlaceholders(obj, keyVault);

    if (nullptr != efc) {
        EDCServerCollection::validateEncryptedFieldInfo(result, *efc);
    }

    // Start Server Side
    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);

    for (auto& payload : serverPayload) {
        payload.count = 1;
    }

    // Finalize document for insert
    auto finalDoc = EDCServerCollection::finalizeForInsert(result, serverPayload);
    ASSERT_EQ(finalDoc[kSafeContent].type(), Array);
    return finalDoc;
}

void assertPayload(BSONElement elem, Operation operation) {
    int len;
    const char* data(elem.binData(len));
    ConstDataRange cdr(data, len);

    const auto& [encryptedType, subCdr] = fromEncryptedConstDataRange(cdr);
    if (operation == Operation::kFind) {
        ASSERT_TRUE(encryptedType == EncryptedBinDataType::kFLE2FindEqualityPayload);
    } else if (operation == Operation::kInsert) {
        ASSERT_TRUE(encryptedType == EncryptedBinDataType::kFLE2EqualityIndexedValue);
    } else {
        FAIL("Not implemented.");
    }
}

void roundTripTest(BSONObj doc, BSONType type, Operation opType, Fle2AlgorithmInt algorithm) {
    auto element = doc.firstElement();
    ASSERT_EQ(element.type(), type);

    TestKeyVault keyVault;

    auto inputDoc = BSON("plainText"
                         << "sample"
                         << "encrypted" << element);

    auto buf = generatePlaceholder(element, opType, algorithm);
    BSONObjBuilder builder;
    builder.append("plainText", "sample");
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());

    auto finalDoc = encryptDocument(builder.obj(), &keyVault);

    ASSERT_EQ(finalDoc["plainText"].type(), String);
    ASSERT_EQ(finalDoc["encrypted"].type(), BinData);
    ASSERT_TRUE(finalDoc["encrypted"].isBinData(BinDataType::Encrypt));

    // TODO : when query enables server side work for Find, remove this
    // if statement.
    if (opType == Operation::kFind && algorithm == Fle2AlgorithmInt::kEquality) {
        assertPayload(finalDoc["encrypted"], opType);
        return;
    }

    // Decrypt document
    auto decryptedDoc = FLEClientCrypto::decryptDocument(finalDoc, &keyVault);

    // Remove this so the round-trip is clean
    decryptedDoc = decryptedDoc.removeField(kSafeContent);

    ASSERT_BSONOBJ_EQ(inputDoc, decryptedDoc);
}

void roundTripTest(BSONObj doc, BSONType type, Operation opType) {
    roundTripTest(doc, type, opType, Fle2AlgorithmInt::kEquality);
    roundTripTest(doc, type, opType, Fle2AlgorithmInt::kUnindexed);
}

void roundTripMultiencrypted(BSONObj doc1,
                             BSONObj doc2,
                             Operation operation1,
                             Operation operation2) {
    auto element1 = doc1.firstElement();
    auto element2 = doc2.firstElement();

    TestKeyVault keyVault;

    auto inputDoc = BSON("plainText"
                         << "sample"
                         << "encrypted1" << element1 << "encrypted2" << element2);

    auto buf1 = generatePlaceholder(element1, operation1, Fle2AlgorithmInt::kEquality, indexKeyId);
    auto buf2 = generatePlaceholder(element2, operation2, Fle2AlgorithmInt::kEquality, indexKey2Id);

    BSONObjBuilder builder;
    builder.append("plaintext", "sample");
    builder.appendBinData("encrypted1", buf1.size(), BinDataType::Encrypt, buf1.data());
    builder.appendBinData("encrypted2", buf2.size(), BinDataType::Encrypt, buf2.data());

    auto finalDoc = encryptDocument(builder.obj(), &keyVault);

    ASSERT_EQ(finalDoc["encrypted1"].type(), BinData);
    ASSERT_TRUE(finalDoc["encrypted1"].isBinData(BinDataType::Encrypt));

    ASSERT_EQ(finalDoc["encrypted2"].type(), BinData);
    ASSERT_TRUE(finalDoc["encrypted2"].isBinData(BinDataType::Encrypt));

    assertPayload(finalDoc["encrypted1"], operation1);
    assertPayload(finalDoc["encrypted2"], operation2);
}

TEST(FLE_EDC, Allowed_Types) {
    const std::vector<std::pair<BSONObj, BSONType>> universallyAllowedObjects{
        {BSON("sample"
              << "value123"),
         String},
        {BSON("sample" << BSONBinData(
                  testValue.data(), testValue.size(), BinDataType::BinDataGeneral)),
         BinData},
        {BSON("sample" << OID()), jstOID},
        {BSON("sample" << false), Bool},
        {BSON("sample" << true), Bool},
        {BSON("sample" << Date_t()), Date},
        {BSON("sample" << BSONRegEx("value1", "value2")), RegEx},
        {BSON("sample" << 123456), NumberInt},
        {BSON("sample" << Timestamp()), bsonTimestamp},
        {BSON("sample" << 12345678901234567LL), NumberLong},
        {BSON("sample" << BSONCode("value")), Code}};

    const std::vector<std::pair<BSONObj, BSONType>> unindexedAllowedObjects{
        {BSON("sample" << 123.456), NumberDouble},
        {BSON("sample" << Decimal128()), NumberDecimal},
        {BSON("sample" << BSON("nested"
                               << "value")),
         Object},
        {BSON("sample" << BSON_ARRAY(1 << 23)), Array},
        {BSON("sample" << BSONDBRef("value1", OID())), DBRef},
        {BSON("sample" << BSONSymbol("value")), Symbol},
        {BSON("sample" << BSONCodeWScope("value",
                                         BSON("code"
                                              << "something"))),
         CodeWScope},
    };


    std::vector<Operation> opTypes{Operation::kInsert, Operation::kFind};

    for (const auto& opType : opTypes) {
        for (const auto& [obj, objType] : universallyAllowedObjects) {
            roundTripTest(obj, objType, opType, Fle2AlgorithmInt::kEquality);
            roundTripTest(obj, objType, opType, Fle2AlgorithmInt::kUnindexed);
        }
        for (const auto& [obj, objType] : unindexedAllowedObjects) {
            roundTripTest(obj, objType, opType, Fle2AlgorithmInt::kUnindexed);
        }
    };

    for (const auto& [obj1, _] : universallyAllowedObjects) {
        for (const auto& [obj2, _] : universallyAllowedObjects) {
            roundTripMultiencrypted(obj1, obj2, Operation::kInsert, Operation::kInsert);
            roundTripMultiencrypted(obj1, obj2, Operation::kInsert, Operation::kFind);
            roundTripMultiencrypted(obj1, obj2, Operation::kFind, Operation::kInsert);
            roundTripMultiencrypted(obj1, obj2, Operation::kFind, Operation::kFind);
        }
    }
}

void illegalBSONType(BSONObj doc, BSONType type, Fle2AlgorithmInt algorithm, int expectCode) {
    auto element = doc.firstElement();
    ASSERT_EQ(element.type(), type);

    TestKeyVault keyVault;

    auto buf = generatePlaceholder(element, Operation::kInsert, algorithm, boost::none);
    BSONObjBuilder builder;
    builder.append("plainText", "sample");
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());
    BSONObj obj = builder.obj();

    ASSERT_THROWS_CODE(
        FLEClientCrypto::transformPlaceholders(obj, &keyVault), DBException, expectCode);
}

void illegalBSONType(BSONObj doc, BSONType type, Fle2AlgorithmInt algorithm) {
    const int expectCode = algorithm == Fle2AlgorithmInt::kEquality ? 6338602 : 6379102;
    illegalBSONType(doc, type, algorithm, expectCode);
}

TEST(FLE_EDC, Disallowed_Types) {
    illegalBSONType(BSON("sample" << 123.456), NumberDouble, Fle2AlgorithmInt::kEquality);
    illegalBSONType(BSON("sample" << Decimal128()), NumberDecimal, Fle2AlgorithmInt::kEquality);

    illegalBSONType(BSON("sample" << MINKEY), MinKey, Fle2AlgorithmInt::kEquality);

    illegalBSONType(BSON("sample" << BSON("nested"
                                          << "value")),
                    Object,
                    Fle2AlgorithmInt::kEquality);
    illegalBSONType(BSON("sample" << BSON_ARRAY(1 << 23)), Array, Fle2AlgorithmInt::kEquality);

    illegalBSONType(BSON("sample" << BSONUndefined), Undefined, Fle2AlgorithmInt::kEquality);
    illegalBSONType(BSON("sample" << BSONUndefined), Undefined, Fle2AlgorithmInt::kUnindexed);
    illegalBSONType(BSON("sample" << BSONNULL), jstNULL, Fle2AlgorithmInt::kEquality);
    illegalBSONType(BSON("sample" << BSONNULL), jstNULL, Fle2AlgorithmInt::kUnindexed);
    illegalBSONType(BSON("sample" << BSONCodeWScope("value",
                                                    BSON("code"
                                                         << "something"))),
                    CodeWScope,
                    Fle2AlgorithmInt::kEquality);
    illegalBSONType(BSON("sample" << MAXKEY), MaxKey, Fle2AlgorithmInt::kEquality);
    illegalBSONType(BSON("sample" << MAXKEY), MaxKey, Fle2AlgorithmInt::kUnindexed);
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


void disallowedEqualityPayloadType(BSONType type) {
    auto doc = BSON("sample" << 123456);
    auto element = doc.firstElement();

    TestKeyVault keyVault;


    auto inputDoc = BSON("plainText"
                         << "sample"
                         << "encrypted" << element);

    auto buf = generatePlaceholder(element, Operation::kInsert);
    BSONObjBuilder builder;
    builder.append("plainText", "sample");
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());
    BSONObj obj = builder.obj();

    auto result = FLEClientCrypto::transformPlaceholders(obj, &keyVault);

    // Since FLEClientCrypto::transformPlaceholders validates the type is correct,
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
    disallowedEqualityPayloadType(CodeWScope);

    disallowedEqualityPayloadType(MaxKey);

    uint8_t fakeBSONType = 42;
    ASSERT_FALSE(isValidBSONType(fakeBSONType));
    disallowedEqualityPayloadType(static_cast<BSONType>(fakeBSONType));
}


TEST(FLE_EDC, ServerSide_Payloads) {
    TestKeyVault keyVault;

    auto doc = BSON("sample" << 123456);
    auto element = doc.firstElement();

    auto value = ConstDataRange(element.value(), element.value() + element.valuesize());

    auto collectionToken = FLELevel1TokenGenerator::generateCollectionsLevel1Token(getIndexKey());
    auto serverEncryptToken =
        FLELevel1TokenGenerator::generateServerDataEncryptionLevel1Token(getIndexKey());
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
    auto inputDoc = BSON(kSafeContent << BSON_ARRAY(1 << 2 << 4) << "encrypted" << element);

    auto buf = generatePlaceholder(element, Operation::kInsert);
    BSONObjBuilder builder;
    builder.append(kSafeContent, BSON_ARRAY(1 << 2 << 4));
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());

    auto finalDoc = encryptDocument(builder.obj(), &keyVault);

    ASSERT_EQ(finalDoc[kSafeContent].type(), Array);
    ASSERT_EQ(finalDoc["encrypted"].type(), BinData);
    ASSERT_TRUE(finalDoc["encrypted"].isBinData(BinDataType::Encrypt));

    // Decrypt document
    auto decryptedDoc = FLEClientCrypto::decryptDocument(finalDoc, &keyVault);

    std::cout << "Final Doc: " << decryptedDoc << std::endl;

    auto elements = finalDoc[kSafeContent].Array();
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

    auto buf = generatePlaceholder(element, Operation::kInsert);
    BSONObjBuilder builder;
    builder.append(kSafeContent, 123456);
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
                "keyId": {
                    "$uuid": "12345678-1234-9876-1234-123456789012"
                },
                "path": "encrypted",
                "bsonType": "string",
                "queries": {
                    "queryType": "equality"
                }
            },
            {
                "keyId": {
                    "$uuid": "12345678-1234-9876-1234-123456789013"
                },
                "path": "nested.encrypted",
                "bsonType": "string",
                "queries": {
                    "queryType": "equality"
                }
            },
            {
                "keyId": {
                    "$uuid": "12345678-1234-9876-1234-123456789014"
                },
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
    auto inputDoc = BSON(kSafeContent << BSON_ARRAY(1 << 2 << 4) << "encrypted" << element);

    auto buf = generatePlaceholder(element, Operation::kInsert);
    BSONObjBuilder builder;
    builder.append(kSafeContent, BSON_ARRAY(1 << 2 << 4));
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());
    {
        BSONObjBuilder sub(builder.subobjStart("nested"));
        auto buf2 = generatePlaceholder(
            element, Operation::kInsert, Fle2AlgorithmInt::kEquality, indexKey2Id);
        sub.appendBinData("encrypted", buf2.size(), BinDataType::Encrypt, buf2.data());
        {
            BSONObjBuilder sub2(sub.subobjStart("nested2"));
            auto buf3 = generatePlaceholder(
                element, Operation::kInsert, Fle2AlgorithmInt::kEquality, indexKey3Id);
            sub2.appendBinData("encrypted", buf3.size(), BinDataType::Encrypt, buf3.data());
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

// Error if the user tries to reuse the same index key across fields
TEST(IndexedFields, DuplicateIndexKeyIds) {
    TestKeyVault keyVault;

    auto doc = BSON("value"
                    << "123456");
    auto element = doc.firstElement();
    auto inputDoc = BSON(kSafeContent << BSON_ARRAY(1 << 2 << 4) << "encrypted" << element);

    auto buf = generatePlaceholder(element, Operation::kInsert);
    BSONObjBuilder builder;
    builder.append(kSafeContent, BSON_ARRAY(1 << 2 << 4));
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());
    {
        BSONObjBuilder sub(builder.subobjStart("nested"));
        sub.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());
    }

    ASSERT_THROWS_CODE(encryptDocument(builder.obj(), &keyVault), DBException, 6371407);
}

TEST(DeleteTokens, Basic) {
    TestKeyVault keyVault;
    NamespaceString ns("test.test");
    EncryptedFieldConfig efc = getTestEncryptedFieldConfig();

    auto obj =
        EncryptionInformationHelpers::encryptionInformationSerializeForDelete(ns, efc, &keyVault);

    std::cout << "Tokens" << obj << std::endl;
}

TEST(DeleteTokens, Fetch) {
    TestKeyVault keyVault;
    NamespaceString ns("test.test");
    EncryptedFieldConfig efc = getTestEncryptedFieldConfig();

    auto obj =
        EncryptionInformationHelpers::encryptionInformationSerializeForDelete(ns, efc, &keyVault);

    auto tokenMap = EncryptionInformationHelpers::getDeleteTokens(
        ns, EncryptionInformation::parse(IDLParserErrorContext("foo"), obj));

    ASSERT_EQ(tokenMap.size(), 2);

    ASSERT(tokenMap.contains("nested.encrypted"));
    ASSERT(tokenMap.contains("encrypted"));
}

TEST(DeleteTokens, CorruptDelete) {
    TestKeyVault keyVault;
    NamespaceString ns("test.test");
    EncryptedFieldConfig efc = getTestEncryptedFieldConfig();

    EncryptionInformation ei;
    ei.setType(1);

    ei.setSchema(BSON(ns.toString() << efc.toBSON()));

    // Missing Delete tokens
    ASSERT_THROWS_CODE(EncryptionInformationHelpers::getDeleteTokens(ns, ei), DBException, 6371308);

    // NSS map is not an object
    ei.setDeleteTokens(BSON(ns.toString() << "str"));

    ASSERT_THROWS_CODE(EncryptionInformationHelpers::getDeleteTokens(ns, ei), DBException, 6371309);

    // Tokens is not a map
    ei.setDeleteTokens(BSON(ns.toString() << BSON("a"
                                                  << "b")));

    ASSERT_THROWS_CODE(EncryptionInformationHelpers::getDeleteTokens(ns, ei), DBException, 6371310);
}

// Verify we can compare two list of tags correctly
TEST(TagDelta, Basic) {
    auto empty = ConstDataRange(nullptr, nullptr);
    auto v1 = ConstDataRange(testValue);
    auto v2 = ConstDataRange(testValue2);
    std::vector<EDCIndexedFields> emptyFields = {};
    std::vector<EDCIndexedFields> origFields = {{empty, "a"}, {empty, "b"}};
    std::vector<EDCIndexedFields> newFields = {{empty, "a"}, {empty, "b"}, {empty, "c"}};
    std::vector<EDCIndexedFields> newFieldsReverse = {{empty, "c"}, {empty, "b"}, {empty, "a"}};
    std::vector<EDCIndexedFields> origFields2 = {{empty, "a"}, {v2, "b"}};
    std::vector<EDCIndexedFields> origFields3 = {{v1, "a"}, {v2, "b"}};
    std::vector<EDCIndexedFields> origFields4 = {{v2, "a"}, {v1, "b"}};

    {
        auto removedFields = EDCServerCollection::getRemovedTags(origFields, origFields);
        ASSERT_EQ(removedFields.size(), 0);
    }

    {
        auto removedFields = EDCServerCollection::getRemovedTags(origFields, newFields);
        ASSERT_EQ(removedFields.size(), 0);
    }

    {
        auto removedFields = EDCServerCollection::getRemovedTags(newFields, origFields);
        ASSERT_EQ(removedFields.size(), 1);
        ASSERT_EQ(removedFields[0].fieldPathName, "c");
    }

    {
        auto removedFields = EDCServerCollection::getRemovedTags(emptyFields, origFields);
        ASSERT_EQ(removedFields.size(), 0);
    }

    {
        auto removedFields = EDCServerCollection::getRemovedTags(newFields, emptyFields);
        ASSERT_EQ(removedFields.size(), 3);
    }

    {
        auto removedFields = EDCServerCollection::getRemovedTags(newFields, newFieldsReverse);
        ASSERT_EQ(removedFields.size(), 0);
    }

    {
        auto removedFields = EDCServerCollection::getRemovedTags(origFields, origFields2);
        ASSERT_EQ(removedFields.size(), 1);
        ASSERT_EQ(removedFields[0].fieldPathName, "b");
    }


    {
        auto removedFields = EDCServerCollection::getRemovedTags(origFields, origFields2);
        ASSERT_EQ(removedFields.size(), 1);
        ASSERT_EQ(removedFields[0].fieldPathName, "b");
    }


    {
        auto removedFields = EDCServerCollection::getRemovedTags(origFields2, origFields3);
        ASSERT_EQ(removedFields.size(), 1);
        ASSERT_EQ(removedFields[0].fieldPathName, "a");
    }

    {
        auto removedFields = EDCServerCollection::getRemovedTags(origFields3, origFields3);
        ASSERT_EQ(removedFields.size(), 0);
    }

    {
        auto removedFields = EDCServerCollection::getRemovedTags(origFields3, origFields4);
        ASSERT_EQ(removedFields.size(), 2);
        ASSERT_EQ(removedFields[0].fieldPathName, "a");
        ASSERT_EQ(removedFields[1].fieldPathName, "b");
    }
}

TEST(EDC, UnindexedEncryptDecrypt) {
    TestKeyVault keyVault;
    FLEUserKeyAndId userKey = keyVault.getUserKeyById(indexKey2Id);

    auto inputDoc = BSON("a"
                         << "sample");
    auto element = inputDoc.firstElement();
    auto const elementData =
        std::vector<uint8_t>(element.value(), element.value() + element.valuesize());

    auto blob = FLE2UnindexedEncryptedValue::serialize(userKey, element);
    ASSERT_EQ(blob[0], 6);

    auto [type, plainText] = FLE2UnindexedEncryptedValue::deserialize(&keyVault, {blob});
    ASSERT_EQ(type, element.type());
    ASSERT_TRUE(
        std::equal(plainText.begin(), plainText.end(), elementData.begin(), elementData.end()));
}

TEST(EDC, ValidateDocument) {
    EncryptedFieldConfig efc = getTestEncryptedFieldConfig();

    TestKeyVault keyVault;

    BSONObjBuilder builder;
    builder.append("plainText", "sample");
    {
        auto doc = BSON("a"
                        << "secret");
        auto element = doc.firstElement();
        auto buf = generatePlaceholder(element, Operation::kInsert);
        builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());
    }
    {
        auto doc = BSON("a"
                        << "top secret");
        auto element = doc.firstElement();

        BSONObjBuilder sub(builder.subobjStart("nested"));
        auto buf = generatePlaceholder(
            element, Operation::kInsert, Fle2AlgorithmInt::kEquality, indexKey2Id);
        builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());
    }
    {
        auto doc = BSON("a"
                        << "bottom secret");
        auto element = doc.firstElement();

        BSONObjBuilder sub(builder.subobjStart("nested"));
        auto buf = generatePlaceholder(element, Operation::kInsert, Fle2AlgorithmInt::kUnindexed);
        builder.appendBinData("notindexed", buf.size(), BinDataType::Encrypt, buf.data());
    }

    auto finalDoc = encryptDocument(builder.obj(), &keyVault, &efc);

    // Positive - Encrypted Doc
    FLEClientCrypto::validateDocument(finalDoc, efc, &keyVault);

    // Positive - Unencrypted Doc
    auto unencryptedDocument = BSON("a" << 123);
    FLEClientCrypto::validateDocument(unencryptedDocument, efc, &keyVault);

    // Remove all tags
    {
        auto testDoc = finalDoc.removeField(kSafeContent);

        ASSERT_THROWS_CODE(
            FLEClientCrypto::validateDocument(testDoc, efc, &keyVault), DBException, 6371506);
    }

    // Remove an encrypted field
    {
        auto testDoc = finalDoc.removeField("encrypted");
        ASSERT_THROWS_CODE(
            FLEClientCrypto::validateDocument(testDoc, efc, &keyVault), DBException, 6371510);
    }

    // Remove a tag
    {
        BSONObj sc2 = BSON(kSafeContent << BSON_ARRAY(finalDoc[kSafeContent].Array()[0]));
        auto testDoc = finalDoc.addFields(sc2);
        ASSERT_THROWS_CODE(
            FLEClientCrypto::validateDocument(testDoc, efc, &keyVault), DBException, 6371516);
    }

    // Make safecontent an int
    {
        BSONObj sc2 = BSON(kSafeContent << 1234);
        auto testDoc = finalDoc.addFields(sc2);
        ASSERT_THROWS_CODE(
            FLEClientCrypto::validateDocument(testDoc, efc, &keyVault), DBException, 6371507);
    }

    // Replace a tag
    {
        PrfBlock block;
        BSONObj sc2 = BSON(kSafeContent
                           << BSON_ARRAY(finalDoc[kSafeContent].Array()[0] << BSONBinData(
                                             &block, sizeof(block), BinDataType::BinDataGeneral)));
        auto testDoc = finalDoc.addFields(sc2);

        ASSERT_THROWS_CODE(
            FLEClientCrypto::validateDocument(testDoc, efc, &keyVault), DBException, 6371510);
    }

    // Wrong tag type
    {
        BSONObj sc2 = BSON(kSafeContent << BSON_ARRAY(123));
        auto testDoc = finalDoc.addFields(sc2);

        ASSERT_THROWS_CODE(
            FLEClientCrypto::validateDocument(testDoc, efc, &keyVault), DBException, 6371515);
    }
}

TEST(EDC, NonMatchingSchema) {
    EncryptedFieldConfig efc = getTestEncryptedFieldConfig();

    TestKeyVault keyVault;

    BSONObjBuilder builder;
    builder.append("plainText", "sample");
    auto doc = BSON("a"
                    << "not really a secret");
    auto element = doc.firstElement();
    auto buf = generatePlaceholder(element, Operation::kInsert);
    builder.appendBinData("not-encrypted", buf.size(), BinDataType::Encrypt, buf.data());

    ASSERT_THROWS_CODE(encryptDocument(builder.obj(), &keyVault, &efc), DBException, 6373601);
}

TEST(EDC, EncryptAlreadyEncryptedData) {
    constexpr StringData testVectors[] = {
        "07b347ede7329f41729dd4004b9d950ff102de64b1925159d2100d58c8d1d0a77bf23a52d30e8861d659e85de2ff96bf8326b3a57134efe5938f439936721dbfa22b02df9df0f63c6453fb2e30ee21b8bab39d4dfb3566926c650fe6995e6caeec025dac818c5a472653876b4a30711c141187236ab5d3dce403aa917d50e432a0ed6f8a685be18af3e2cd21f6b1aeee0e835de13b33fa76eace42527207db517b9e3dce5d0a0d9e25853f612e198a34b37adfce8cfeb673ef779c81c80412a96460e53fb65b0504651d55a4f329a8dc72aaeee93d1b62bf0b9564a71a"_sd,
        "07"_sd,
        "00"_sd,
        "676172626167650a"_sd,    // "garbage"
        "07676172626167650a"_sd,  // "\x07garbage"
        "06676172626167650a"_sd,  // "\x06garbage"
    };

    EncryptedFieldConfig efc = getTestEncryptedFieldConfig();
    TestKeyVault keyVault;

    for (const auto& s : testVectors) {
        BSONObjBuilder builder;
        builder.append("plainText", "sample");

        BSONObjBuilder builder1;
        auto data = hexblob::decode(s);
        builder1.appendBinData("a", data.length(), BinDataType::Encrypt, data.data());
        auto doc = builder1.obj();

        auto element = doc.firstElement();
        auto buf = generatePlaceholder(element, Operation::kInsert);
        builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());

        ASSERT_THROWS_CODE(encryptDocument(builder.obj(), &keyVault, &efc), DBException, 6409401);
    }
}

TEST(FLE1, EncryptAlreadyEncryptedDataLegacy) {
    BSONObjBuilder builder;
    builder.append("plainText", "sample");

    BSONObjBuilder builder1;
    auto data = hexblob::decode("676172626167650a"_sd);
    builder1.appendBinData("a", data.length(), BinDataType::Encrypt, data.data());
    auto doc = builder1.obj();

    auto valueElem = doc.firstElement();
    BSONType bsonType = valueElem.type();
    ConstDataRange plaintext(valueElem.value(), valueElem.valuesize());
    UUID uuid = UUID::gen();
    auto symmKey =
        std::make_shared<SymmetricKey>(crypto::aesGenerate(crypto::sym256KeySize, "testID"));
    size_t cipherLength = crypto::aeadCipherOutputLength(plaintext.length());
    ASSERT_THROWS_CODE(
        [&] {
            FLEEncryptionFrame dataFrame(
                symmKey, FleAlgorithmInt::kDeterministic, uuid, bsonType, plaintext, cipherLength);
        }(),
        DBException,
        6409402);
}

BSONObj encryptUpdateDocument(BSONObj obj, FLEKeyVault* keyVault) {
    auto result = FLEClientCrypto::transformPlaceholders(obj, keyVault);

    // Start Server Side
    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);

    for (auto& payload : serverPayload) {
        payload.count = 1;
    }

    return EDCServerCollection::finalizeForUpdate(result, serverPayload);
}

// Test update with no $push
TEST(FLE_Update, Basic) {
    TestKeyVault keyVault;

    auto doc = BSON("value"
                    << "123456");
    auto element = doc.firstElement();

    auto buf = generatePlaceholder(element, Operation::kInsert);
    auto inputDoc = BSON(
        "$set" << BSON("encrypted" << BSONBinData(buf.data(), buf.size(), BinDataType::Encrypt)));
    auto finalDoc = encryptUpdateDocument(inputDoc, &keyVault);

    std::cout << finalDoc << std::endl;

    ASSERT_TRUE(finalDoc["$set"]["encrypted"].isBinData(BinDataType::Encrypt));
    ASSERT_TRUE(finalDoc["$push"][kSafeContent]["$each"].type() == Array);
    ASSERT_EQ(finalDoc["$push"][kSafeContent]["$each"].Array().size(), 1);
    ASSERT_TRUE(
        finalDoc["$push"][kSafeContent]["$each"].Array()[0].isBinData(BinDataType::BinDataGeneral));
}

// Test update with no crypto
TEST(FLE_Update, Empty) {
    TestKeyVault keyVault;

    auto inputDoc = BSON("$set" << BSON("count" << 1));
    auto finalDoc = encryptUpdateDocument(inputDoc, &keyVault);

    std::cout << finalDoc << std::endl;

    ASSERT_EQ(finalDoc["$set"]["count"].type(), NumberInt);
    ASSERT(finalDoc["$push"].eoo());
}

TEST(FLE_Update, BadPush) {
    TestKeyVault keyVault;

    auto doc = BSON("value"
                    << "123456");
    auto element = doc.firstElement();

    auto buf = generatePlaceholder(element, Operation::kInsert);
    auto inputDoc = BSON(
        "$push" << 123 << "$set"
                << BSON("encrypted" << BSONBinData(buf.data(), buf.size(), BinDataType::Encrypt)));
    ASSERT_THROWS_CODE(encryptUpdateDocument(inputDoc, &keyVault), DBException, 6371511);
}

TEST(FLE_Update, PushToSafeContent) {
    TestKeyVault keyVault;

    auto doc = BSON("value"
                    << "123456");
    auto element = doc.firstElement();

    auto buf = generatePlaceholder(element, Operation::kInsert);
    auto inputDoc = BSON(
        "$push" << 123 << "$set"
                << BSON("encrypted" << BSONBinData(buf.data(), buf.size(), BinDataType::Encrypt)));
    ASSERT_THROWS_CODE(encryptUpdateDocument(inputDoc, &keyVault), DBException, 6371511);
}

TEST(FLE_Update, PushToOtherfield) {
    TestKeyVault keyVault;

    auto doc = BSON("value"
                    << "123456");
    auto element = doc.firstElement();

    auto buf = generatePlaceholder(element, Operation::kInsert);
    auto inputDoc = BSON(
        "$push" << BSON("abc" << 123) << "$set"
                << BSON("encrypted" << BSONBinData(buf.data(), buf.size(), BinDataType::Encrypt)));
    auto finalDoc = encryptUpdateDocument(inputDoc, &keyVault);
    std::cout << finalDoc << std::endl;

    ASSERT_TRUE(finalDoc["$set"]["encrypted"].isBinData(BinDataType::Encrypt));
    ASSERT_TRUE(finalDoc["$push"]["abc"].type() == NumberInt);
    ASSERT_TRUE(finalDoc["$push"][kSafeContent]["$each"].type() == Array);
    ASSERT_EQ(finalDoc["$push"][kSafeContent]["$each"].Array().size(), 1);
    ASSERT_TRUE(
        finalDoc["$push"][kSafeContent]["$each"].Array()[0].isBinData(BinDataType::BinDataGeneral));
}

TEST(FLE_Update, PullTokens) {
    TestKeyVault keyVault;
    NamespaceString ns("test.test");
    EncryptedFieldConfig efc = getTestEncryptedFieldConfig();

    auto obj =
        EncryptionInformationHelpers::encryptionInformationSerializeForDelete(ns, efc, &keyVault);

    auto tokenMap = EncryptionInformationHelpers::getDeleteTokens(
        ns, EncryptionInformation::parse(IDLParserErrorContext("foo"), obj));

    ASSERT_EQ(tokenMap.size(), 2);

    ASSERT(tokenMap.contains("nested.encrypted"));
    ASSERT(tokenMap.contains("encrypted"));


    auto doc = BSON("value"
                    << "123456");
    auto element = doc.firstElement();
    auto inputDoc = BSON(kSafeContent << BSON_ARRAY(1 << 2 << 4) << "encrypted" << element);

    auto buf = generatePlaceholder(element, Operation::kInsert);
    BSONObjBuilder builder;
    builder.append(kSafeContent, BSON_ARRAY(1 << 2 << 4));
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());
    {
        BSONObjBuilder sub(builder.subobjStart("nested"));
        auto buf2 = generatePlaceholder(
            element, Operation::kInsert, Fle2AlgorithmInt::kEquality, indexKey2Id);
        sub.appendBinData("encrypted", buf2.size(), BinDataType::Encrypt, buf2.data());
    }
    auto encDoc = encryptDocument(builder.obj(), &keyVault);

    auto removedFields = EDCServerCollection::getEncryptedIndexedFields(encDoc);

    auto pullUpdate1 = EDCServerCollection::generateUpdateToRemoveTags(removedFields, tokenMap);

    std::cout << "PULL: " << pullUpdate1 << std::endl;

    ASSERT_EQ(pullUpdate1["$pull"].type(), Object);
    ASSERT_EQ(pullUpdate1["$pull"][kSafeContent].type(), Object);
    ASSERT_EQ(pullUpdate1["$pull"][kSafeContent]["$in"].type(), Array);

    // Verify we fail when we are missing tokens for affected fields
    tokenMap.clear();
    ASSERT_THROWS_CODE(EDCServerCollection::generateUpdateToRemoveTags(removedFields, tokenMap),
                       DBException,
                       6371513);
}

TEST(CompactionHelpersTest, parseCompactionTokensTest) {
    auto result = CompactionHelpers::parseCompactionTokens(BSONObj());
    ASSERT(result.empty());

    ECOCToken token1(
        decodePrf("7076c7b05fb4be4fe585eed930b852a6d088a0c55f3c96b50069e8a26ebfb347"_sd));
    ECOCToken token2(
        decodePrf("6ebfb347576b4be4fe585eed96d088a0c55f3c96b50069e8a230b852a05fb4be"_sd));
    BSONObjBuilder builder;
    builder.appendBinData(
        "a.b.c", token1.toCDR().length(), BinDataType::BinDataGeneral, token1.toCDR().data());
    builder.appendBinData(
        "x.y", token2.toCDR().length(), BinDataType::BinDataGeneral, token2.toCDR().data());
    result = CompactionHelpers::parseCompactionTokens(builder.obj());

    ASSERT(result.size() == 2);
    ASSERT(result[0].fieldPathName == "a.b.c");
    ASSERT(result[0].token == token1);
    ASSERT(result[1].fieldPathName == "x.y");
    ASSERT(result[1].token == token2);

    ASSERT_THROWS_CODE(CompactionHelpers::parseCompactionTokens(BSON("foo"
                                                                     << "bar")),
                       DBException,
                       6346801);
}

TEST(CompactionHelpersTest, validateCompactionTokensTest) {
    EncryptedFieldConfig efc = getTestEncryptedFieldConfig();

    BSONObjBuilder builder;
    for (auto& field : efc.getFields()) {
        // validate fails until all fields are present
        ASSERT_THROWS_CODE(CompactionHelpers::validateCompactionTokens(efc, builder.asTempObj()),
                           DBException,
                           6346806);

        // validate doesn't care about the value, so this is fine
        builder.append(field.getPath(), "foo");
    }
    CompactionHelpers::validateCompactionTokens(efc, builder.asTempObj());

    // validate OK if obj has extra fields
    builder.append("abc.xyz", "foo");
    CompactionHelpers::validateCompactionTokens(efc, builder.obj());
}

std::vector<ECCDocument> pairsToECCDocuments(
    const std::vector<std::pair<uint64_t, uint64_t>>& pairs) {
    std::vector<ECCDocument> output;
    std::transform(pairs.begin(), pairs.end(), std::back_inserter(output), [](auto& pair) {
        return ECCDocument{ECCValueType::kNormal, pair.first, pair.second};
    });
    return output;
}

TEST(CompactionHelpersTest, mergeECCDocumentsTest) {
    std::vector<ECCDocument> input, output, expected;

    // Test empty input
    output = CompactionHelpers::mergeECCDocuments(input);
    ASSERT(output.empty());

    // Test single pair
    input = pairsToECCDocuments({{15, 20}});
    output = CompactionHelpers::mergeECCDocuments(input);
    ASSERT(output == input);

    // Test input with no gaps
    input = pairsToECCDocuments({{15, 20}, {13, 13}, {1, 6}, {7, 12}, {14, 14}});
    output = CompactionHelpers::mergeECCDocuments(input);
    ASSERT_EQ(output.size(), 1);
    ASSERT_EQ(output.front().start, 1);
    ASSERT_EQ(output.front().end, 20);

    // Test input with gaps; nothing is merged
    input = pairsToECCDocuments({{5, 5}, {12, 16}, {9, 9}, {23, 45}});
    output = CompactionHelpers::mergeECCDocuments(input);
    ASSERT(output == input);

    // Test input with gaps; at least one merged
    input = pairsToECCDocuments({{5, 5}, {12, 16}, {6, 9}, {17, 23}, {45, 45}});
    expected = pairsToECCDocuments({{5, 9}, {12, 23}, {45, 45}});
    output = CompactionHelpers::mergeECCDocuments(input);
    ASSERT(output == expected);
}

TEST(CompactionHelpersTest, countDeletedTest) {
    ASSERT_EQ(CompactionHelpers::countDeleted({}), 0);

    auto input = pairsToECCDocuments({{15, 20}, {13, 13}, {1, 6}, {7, 12}, {14, 14}});
    ASSERT_EQ(CompactionHelpers::countDeleted(input), 20);
}

}  // namespace mongo
