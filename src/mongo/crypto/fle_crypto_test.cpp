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
#include <string>
#include <tuple>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/matcher/schema/encrypt_schema_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/platform/decimal128.h"
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
        ASSERT_EQ(swDoc.getValue().pos, 123);
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
        ASSERT_EQ(swDoc.getValue().pos, 123456789);
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

    uint64_t i = ESCCollection::emuBinary(&coll, escTwiceTag, escTwiceValue);

    std::cout << "i: " << i << std::endl;
    ASSERT_EQ(i, 5);
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

    uint64_t i = ESCCollection::emuBinary(&coll, escTwiceTag, escTwiceValue);

    ASSERT_EQ(i, 13);

    i = ESCCollection::emuBinary(&coll, escTwiceTag2, escTwiceValue2);

    ASSERT_EQ(i, 5);
}


}  // namespace mongo
