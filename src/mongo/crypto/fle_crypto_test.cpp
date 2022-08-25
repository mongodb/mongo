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
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
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
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/db/matcher/schema/encrypt_schema_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/decimal128.h"
#include "mongo/rpc/object_check.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

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

TEST(FLETokens, TestVectors) {

    // Level 1
    auto collectionToken = FLELevel1TokenGenerator::generateCollectionsLevel1Token(getIndexKey());

    ASSERT_EQUALS(CollectionsLevel1Token(decodePrf(
                      "BD53ACAC665EDD01E0CA30CB648B2B8F4967544047FD4E7D12B1A9BF07339928"_sd)),
                  collectionToken);

    ASSERT_EQUALS(ServerDataEncryptionLevel1Token(decodePrf(
                      "EB9A73F7912D86A4297E81D2F675AF742874E4057E3A890FEC651A23EEE3F3EC"_sd)),
                  FLELevel1TokenGenerator::generateServerDataEncryptionLevel1Token(getIndexKey()));

    // Level 2
    auto edcToken = FLECollectionTokenGenerator::generateEDCToken(collectionToken);
    ASSERT_EQUALS(
        EDCToken(decodePrf("82B0AB0F8F1D31AEB6F4DBC915EF17CBA2FE21E36EC436984EB63BECEC173831"_sd)),
        edcToken);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(collectionToken);
    ASSERT_EQUALS(
        ESCToken(decodePrf("279C575B52B73677EEF07D9C1126EBDF08C35369570A9B75E44A9AFDCCA96B6D"_sd)),
        escToken);
    auto eccToken = FLECollectionTokenGenerator::generateECCToken(collectionToken);
    ASSERT_EQUALS(
        ECCToken(decodePrf("C58F671F04A8CFDD8FB1F718F563139F1286D7950E97C0C4A94EDDF0EDB127FE"_sd)),
        eccToken);
    ASSERT_EQUALS(
        ECOCToken(decodePrf("9E837ED3926CB8ED680E0E7DCB2A481A3E398BE7851FA1CE4D738FA5E67FFCC9"_sd)),
        FLECollectionTokenGenerator::generateECOCToken(collectionToken));


    // Level 3
    std::vector<uint8_t> sampleValue = {0xc0, 0x7c, 0x0d, 0xf5, 0x12, 0x57, 0x94, 0x8e,
                                        0x1a, 0x0f, 0xc7, 0x0d, 0xd4, 0x56, 0x8e, 0x3a,
                                        0xf9, 0x9b, 0x23, 0xb3, 0x43, 0x4c, 0x98, 0x58,
                                        0x23, 0x7c, 0xa7, 0xdb, 0x62, 0xdb, 0x97, 0x66};

    auto edcDataToken =
        FLEDerivedFromDataTokenGenerator::generateEDCDerivedFromDataToken(edcToken, sampleValue);
    ASSERT_EQUALS(EDCDerivedFromDataToken(decodePrf(
                      "CEA098AA664E578D4E9CE05B50ADD15DF2F0316CD5CCB08E720C61D8C7580E2A"_sd)),
                  edcDataToken);

    auto escDataToken =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, sampleValue);
    ASSERT_EQUALS(ESCDerivedFromDataToken(decodePrf(
                      "DE6A1AC292BC62094C33E94647B044B9B10514317B75F4128DDA2E0FB686704F"_sd)),
                  escDataToken);

    auto eccDataToken =
        FLEDerivedFromDataTokenGenerator::generateECCDerivedFromDataToken(eccToken, sampleValue);
    ASSERT_EQUALS(ECCDerivedFromDataToken(decodePrf(
                      "9A95D4F44734447E3F0266D1629513A0B7698CCE8C1524F329CE7970627FFD06"_sd)),
                  eccDataToken);

    // Level 4
    FLECounter counter = 1234567890;

    auto edcDataCounterToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateEDCDerivedFromDataTokenAndContentionFactorToken(edcDataToken, counter);
    ASSERT_EQUALS(EDCDerivedFromDataTokenAndContentionFactorToken(decodePrf(
                      "D8CC38AE6A64BD1BF195A2D35734C13AF2B1729AD1052A81BE00BF29C67A696E"_sd)),
                  edcDataCounterToken);


    auto escDataCounterToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateESCDerivedFromDataTokenAndContentionFactorToken(escDataToken, counter);
    ASSERT_EQUALS(ESCDerivedFromDataTokenAndContentionFactorToken(decodePrf(
                      "8AAF04CBA6DC16BFB37CADBA43DCA66C183634CB3DA278DE174556AE6E17CEBB"_sd)),
                  escDataCounterToken);


    auto eccDataCounterToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateECCDerivedFromDataTokenAndContentionFactorToken(eccDataToken, counter);
    ASSERT_EQUALS(ECCDerivedFromDataTokenAndContentionFactorToken(decodePrf(
                      "E9580F805E0D07AF384EBA185384F28A49C3DB93AFA4A187A1F4DA129271D82C"_sd)),
                  eccDataCounterToken);


    // Level 5
    auto edcTwiceToken =
        FLETwiceDerivedTokenGenerator::generateEDCTwiceDerivedToken(edcDataCounterToken);
    ASSERT_EQUALS(EDCTwiceDerivedToken(decodePrf(
                      "B39A7EC33FD976EFB8EEBBBF3A265A933E2128D709BB88C77E3D42AA735F697C"_sd)),
                  edcTwiceToken);

    auto escTwiceTagToken =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escDataCounterToken);
    ASSERT_EQUALS(ESCTwiceDerivedTagToken(decodePrf(
                      "D6F76A9D4767E0889B709517C8CF0412D81874AEB6E6CEBFBDDFF7B013EB7154"_sd)),
                  escTwiceTagToken);
    auto escTwiceValueToken =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(escDataCounterToken);
    ASSERT_EQUALS(ESCTwiceDerivedValueToken(decodePrf(
                      "53F0A51A43447B9881D5E79BA4C5F78E80BC2BC6AA42B00C81079EBF4C9D5A7C"_sd)),
                  escTwiceValueToken);


    auto eccTwiceTagToken =
        FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedTagToken(eccDataCounterToken);
    ASSERT_EQUALS(ECCTwiceDerivedTagToken(decodePrf(
                      "5DD9F09757BE35BB33FFAF6FC5CDFC649248E59AEA9FF7D9E2A9F36B6F5A6152"_sd)),
                  eccTwiceTagToken);
    auto eccTwiceValueToken =
        FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedValueToken(eccDataCounterToken);
    ASSERT_EQUALS(ECCTwiceDerivedValueToken(decodePrf(
                      "EFA5746DB796DAC6FAACB7E5F28DB53B333588A43131F0C026B19D2B1215EAE2"_sd)),
                  eccTwiceValueToken);

    // Unindexed field decryption
    // Encryption can not be generated using test vectors because IV is random

    TestKeyVault keyVault;
    const std::string uxCiphertext = hexblob::decode(
        "06ABCDEFAB12349876123412345678901202F2CE7FDD0DECD5442CC98C10B9138741785173E323132982740496768877A3BA46581CED4A34031B1174B5C524C15BAAE687F88C29FC71F40A32BCD53D63CDA0A6646E8677E167BB3A933529F5B519CFE255BBC323D943B4F105"_sd);
    auto [uxBsonType, uxPlaintext] =
        FLE2UnindexedEncryptedValue::deserialize(&keyVault, ConstDataRange(uxCiphertext));
    ASSERT_EQUALS(uxBsonType, BSONType::String);
    ASSERT_EQUALS(
        hexblob::encode(uxPlaintext.data(), uxPlaintext.size()),
        "260000004C6F7279207761732061206D6F75736520696E2061206269672062726F776E20686F75736500");

    // Equality indexed field decryption
    // Encryption can not be generated using test vectors because IV is random

    const std::string ixCiphertext = hexblob::decode(
        "000000000000000000000000000000000297044B8E1B5CF4F9052EDB50236A343597C418A74352F98357A77E0D4299C04151CBEC24A5D5349A5A5EAA1FE334154FEEB6C8E7BD636089904F76950B2184D146792CBDF9179FFEDDB7D90FC257BB13DCB3E731182A447E2EF1BE7A2AF13DC9362701BABDE0B5E78CF4A92227D5B5D1E1556E75BAB5B4E9F5CEFEA3BA3E3D5D31D11B20619437A30550EFF5B602357567CF05058E4F84A103293F70302F3A50667642DD0325D194A197"_sd);
    ServerDataEncryptionLevel1Token serverEncryptToken(
        decodePrf("EB9A73F7912D86A4297E81D2F675AF742874E4057E3A890FEC651A23EEE3F3EC"_sd));

    auto swServerPayload = FLE2IndexedEqualityEncryptedValue::decryptAndParse(
        serverEncryptToken, ConstDataRange(ixCiphertext));
    ASSERT_OK(swServerPayload.getStatus());

    auto cdrEqualHex = [](ConstDataRange cdr, const StringData hex) -> bool {
        const std::string s = hexblob::decode(hex);
        return cdr.length() == s.size() && std::equal(s.begin(), s.end(), cdr.data<char>());
    };

    auto sp = swServerPayload.getValue();
    ASSERT(cdrEqualHex(sp.edc.toCDR(),
                       "97C8DFE394D80A4EE335E3F9FDC024D18BE4B92F9444FCA316FF9896D7BF455D"_sd));
    ASSERT(cdrEqualHex(sp.esc.toCDR(),
                       "EBB22F74BE0FA4AD863188D3F33AF0B95CB4CA4ED0091E1A43513DB20E9D59AE"_sd));
    ASSERT(cdrEqualHex(sp.ecc.toCDR(),
                       "A1DF0BB04C977BD4BC0B487FFFD2E3BBB96078354DE9F204EE5872BB10F01971"_sd));
    ASSERT_EQ(sp.count, 123456);
    ASSERT(cdrEqualHex(
        sp.clientEncryptedValue,
        "260000004C6F7279207761732061206D6F75736520696E2061206269672062726F776E20686F75736500"_sd));
}

TEST(FLETokens, TestVectorESCCollectionDecryptDocument) {
    ESCTwiceDerivedTagToken escTwiceTag(
        decodePrf("B1C4E1C67F4AB83DE7632B801BDD198D65401B17EC633EB4D608DE97FAFCE02B"_sd));
    ESCTwiceDerivedValueToken escTwiceValue(
        decodePrf("E2E3F08343FD16BCB36927FFA39C7BCC6AA1E33E6E553DF9FE445ABB988D30D1"_sd));

    BSONObj doc = fromjson(R"({
            "_id": {
                "$binary": {
                    "base64": "bdK0MLySL7lEaje7JHIWvvpx/AQWZID2kW47M1XLFUg=",
                    "subType": "0"
                }
            },
            "value": {
                "$binary": {
                    "base64": "+17srnmE1l+T1np0IJxoeLRzD1ac5st9k/a0YHxeqk0=",
                    "subType": "0"
                }
            }
        })");

    auto swDoc = ESCCollection::decryptDocument(escTwiceValue, doc);
    ASSERT_OK(swDoc.getStatus());
    ASSERT_EQ(swDoc.getValue().compactionPlaceholder, false);
    ASSERT_EQ(swDoc.getValue().position, 0);
    ASSERT_EQ(swDoc.getValue().count, 123456789);
}

TEST(FLETokens, TestVectorECCCollectionDecryptDocument) {
    ECCTwiceDerivedTagToken twiceTag(
        decodePrf("8879748219186CAC6B5E77D664A05C4BA2C7690F09ACC16B8E9910B80FF4B5AB"_sd));
    ECCTwiceDerivedValueToken twiceValue(
        decodePrf("F868EB46AA38963658E453DE05B2955225CB00C96B72975DACF9D837C8189FA2"_sd));

    BSONObj doc = fromjson(R"({
            "_id": {
                "$binary": {
                    "base64": "TTB8rMJipFwpSMbWMf3Rpx8RuRP4Fnc6bJl1tdMc84A=",
                    "subType": "0"
                }
            },
            "value": {
                "$binary": {
                    "base64": "anHlFVy/XbIDENbKPUVf5OgPv2fkt3JBxYAUGTStAj4=",
                    "subType": "0"
                }
            }
        })");

    auto swDoc = ECCCollection::decryptDocument(twiceValue, doc);

    ASSERT_OK(swDoc.getStatus());
    ASSERT(swDoc.getValue().valueType == ECCValueType::kNormal);
    ASSERT_EQ(swDoc.getValue().start, 123456789);
    ASSERT_EQ(swDoc.getValue().end, 123456789);
}

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
        if (_overrideCount) {
            return *_overrideCount;
        }

        return _docs.size();
    }

    void setOverrideCount(int64_t count) {
        _overrideCount = count;
    }

private:
    std::vector<BSONObj> _docs;
    boost::optional<int64_t> _overrideCount;
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


    // Test with various fake counts to ensure enumBinary works with bad estimates and the original
    // exact count.
    int64_t origCount = coll.getDocumentCount();
    std::vector<int64_t> testVectors{0, 2, 3, 13, 500, origCount};

    for (const auto v : testVectors) {
        coll.setOverrideCount(v);
        auto i = ESCCollection::emuBinary(coll, escTwiceTag, escTwiceValue);

        ASSERT_TRUE(i.has_value());
        ASSERT_EQ(i.value(), 5);
    }
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

    // Test with various fake counts to ensure enumBinary works with bad estimates and the original
    // exact count.
    int64_t origCount = coll.getDocumentCount();
    std::vector<int64_t> testVectors{0, 2, 5, 13, 19, 500, origCount};

    for (const auto v : testVectors) {
        coll.setOverrideCount(v);
        auto i = ESCCollection::emuBinary(coll, escTwiceTag, escTwiceValue);

        ASSERT_TRUE(i.has_value());
        ASSERT_EQ(i.value(), 13);

        i = ESCCollection::emuBinary(coll, escTwiceTag2, escTwiceValue2);

        ASSERT_TRUE(i.has_value());
        ASSERT_EQ(i.value(), 5);
    }
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
    mongo::Fle2AlgorithmInt algorithm = Fle2AlgorithmInt::kEquality,
    boost::optional<UUID> key = boost::none,
    uint64_t contention = 0) {
    FLE2EncryptionPlaceholder ep;

    if (operation == Operation::kFind) {
        ep.setType(mongo::Fle2PlaceholderType::kFind);
    } else if (operation == Operation::kInsert) {
        ep.setType(mongo::Fle2PlaceholderType::kInsert);
    }

    ep.setAlgorithm(algorithm);
    ep.setUserKeyId(userKeyId);
    ep.setIndexKeyId(key.value_or(indexKeyId));

    FLE2RangeInsertSpec insertSpec;
    // Set a default lower and upper bound
    auto lowerDoc = BSON("lb" << 0);
    insertSpec.setLowerBound(lowerDoc.firstElement());
    auto upperDoc = BSON("ub" << 1234567890123456789LL);

    insertSpec.setUpperBound(upperDoc.firstElement());
    insertSpec.setValue(value);
    auto specDoc = BSON("s" << insertSpec.toBSON());

    FLE2RangeSpec findSpec;
    findSpec.setMin(lowerDoc.firstElement());
    findSpec.setMinIncluded(true);
    findSpec.setMax(upperDoc.firstElement());
    findSpec.setMaxIncluded(true);
    auto findDoc = BSON("s" << findSpec.toBSON());

    if (algorithm == Fle2AlgorithmInt::kRange) {
        if (operation == Operation::kFind) {
            ep.setValue(IDLAnyType(findDoc.firstElement()));
        } else if (operation == Operation::kInsert) {
            ep.setValue(IDLAnyType(specDoc.firstElement()));
        }
        ep.setSparsity(0);
    } else {
        ep.setValue(value);
    }
    ep.setMaxContentionCounter(contention);


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
        EDCServerCollection::validateEncryptedFieldInfo(result, *efc, false);
    }

    // Start Server Side
    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);

    for (auto& payload : serverPayload) {
        if (payload.payload.getEdgeTokenSet().has_value()) {
            for (size_t i = 0; i < payload.payload.getEdgeTokenSet()->size(); i++) {
                payload.counts.push_back(1);
            }
        }

        payload.counts.push_back(1);
    }

    // Finalize document for insert
    auto finalDoc = EDCServerCollection::finalizeForInsert(result, serverPayload);
    ASSERT_EQ(finalDoc[kSafeContent].type(), Array);
    return finalDoc;
}

void assertPayload(BSONElement elem, EncryptedBinDataType type) {
    int len;
    const char* data(elem.binData(len));
    ConstDataRange cdr(data, len);

    const auto& [encryptedType, subCdr] = fromEncryptedConstDataRange(cdr);
    ASSERT_TRUE(encryptedType == type);
}

void assertPayload(BSONElement elem, Operation operation) {
    if (operation == Operation::kFind) {
        assertPayload(elem, EncryptedBinDataType::kFLE2FindEqualityPayload);
    } else if (operation == Operation::kInsert) {
        assertPayload(elem, EncryptedBinDataType::kFLE2EqualityIndexedValue);
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


    // Decrypt document
    auto decryptedDoc = FLEClientCrypto::decryptDocument(finalDoc, &keyVault);

    // Remove this so the round-trip is clean
    decryptedDoc = decryptedDoc.removeField(kSafeContent);

    if (opType == Operation::kFind) {
        assertPayload(finalDoc["encrypted"],
                      algorithm == Fle2AlgorithmInt::kEquality
                          ? EncryptedBinDataType::kFLE2FindEqualityPayload
                          : EncryptedBinDataType::kFLE2FindRangePayload);
    } else {
        ASSERT_BSONOBJ_EQ(inputDoc, decryptedDoc);
    }
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

// Used to generate the test data for the ExpressionFLETest in expression_test.cpp
TEST(FLE_EDC, PrintTest) {
    auto doc = BSON("value" << 1);
    auto element = doc.firstElement();

    TestKeyVault keyVault;

    auto inputDoc = BSON("plainText"
                         << "sample"
                         << "encrypted" << element);

    {
        auto buf = generatePlaceholder(element, Operation::kInsert, Fle2AlgorithmInt::kEquality);
        BSONObjBuilder builder;
        builder.append("plainText", "sample");
        builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());

        auto finalDoc = encryptDocument(builder.obj(), &keyVault);

        std::cout << finalDoc.jsonString() << std::endl;
    }

    {
        auto buf = generatePlaceholder(
            element, Operation::kInsert, Fle2AlgorithmInt::kEquality, boost::none, 50);
        BSONObjBuilder builder;
        builder.append("plainText", "sample");
        builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());

        auto finalDoc = encryptDocument(builder.obj(), &keyVault);

        std::cout << finalDoc.jsonString() << std::endl;
    }
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
            if (opType == Operation::kInsert) {
                roundTripTest(obj, objType, opType, Fle2AlgorithmInt::kUnindexed);
            }
        }
    };

    for (const auto& [obj, objType] : unindexedAllowedObjects) {
        roundTripTest(obj, objType, Operation::kInsert, Fle2AlgorithmInt::kUnindexed);
    }

    for (const auto& [obj1, _] : universallyAllowedObjects) {
        for (const auto& [obj2, _] : universallyAllowedObjects) {
            roundTripMultiencrypted(obj1, obj2, Operation::kInsert, Operation::kInsert);
            roundTripMultiencrypted(obj1, obj2, Operation::kInsert, Operation::kFind);
            roundTripMultiencrypted(obj1, obj2, Operation::kFind, Operation::kInsert);
            roundTripMultiencrypted(obj1, obj2, Operation::kFind, Operation::kFind);
        }
    }
}

TEST(FLE_EDC, Range_Allowed_Types) {

    const std::vector<std::pair<BSONObj, BSONType>> rangeAllowedObjects{
        {BSON("sample" << 123.456), NumberDouble},
        {BSON("sample" << Decimal128()), NumberDecimal},
        {BSON("sample" << 123456), NumberInt},
        {BSON("sample" << 12345678901234567LL), NumberLong},
        {BSON("sample" << Date_t::fromMillisSinceEpoch(12345)), Date},
    };

    std::vector<Operation> opTypes{Operation::kInsert, Operation::kFind};

    for (const auto& opType : opTypes) {
        for (const auto& [obj, objType] : rangeAllowedObjects) {
            roundTripTest(obj, objType, opType, Fle2AlgorithmInt::kRange);
        }
    };
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

void illegalRangeBSONType(BSONObj doc, BSONType type) {
    illegalBSONType(doc, type, Fle2AlgorithmInt::kRange, ErrorCodes::TypeMismatch);
}

TEST(FLE_EDC, Range_Disallowed_Types) {

    const std::vector<std::pair<BSONObj, BSONType>> disallowedObjects{
        {BSON("sample"
              << "value123"),
         String},
        {BSON("sample" << BSONBinData(
                  testValue.data(), testValue.size(), BinDataType::BinDataGeneral)),
         BinData},
        {BSON("sample" << OID()), jstOID},
        {BSON("sample" << false), Bool},
        {BSON("sample" << true), Bool},
        {BSON("sample" << BSONRegEx("value1", "value2")), RegEx},
        {BSON("sample" << Timestamp()), bsonTimestamp},
        {BSON("sample" << BSONCode("value")), Code},
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
        {BSON("sample" << MINKEY), MinKey},
        {BSON("sample" << MAXKEY), MaxKey},
    };

    for (const auto& typePair : disallowedObjects) {
        illegalRangeBSONType(typePair.first, typePair.second);
    }

    illegalBSONType(BSON("sample" << BSONNULL), jstNULL, Fle2AlgorithmInt::kRange, 40414);
    illegalBSONType(BSON("sample" << BSONUndefined), Undefined, Fle2AlgorithmInt::kRange, 40414);
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
    iupayload.setValue(value);
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
    ASSERT_EQ(serverPayload.clientEncryptedValue.size(), value.length());
    ASSERT(std::equal(serverPayload.clientEncryptedValue.begin(),
                      serverPayload.clientEncryptedValue.end(),
                      value.data<uint8_t>()));
}

TEST(FLE_EDC, ServerSide_Range_Payloads) {
    TestKeyVault keyVault;

    auto doc = BSON("sample" << 3);
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
    iupayload.setValue(value);
    iupayload.setType(element.type());

    std::vector<EdgeTokenSet> tokens;
    EdgeTokenSet ets;
    ets.setEdcDerivedToken(edcDatakey.toCDR());
    ets.setEscDerivedToken(escDatakey.toCDR());
    ets.setEccDerivedToken(eccDatakey.toCDR());
    ets.setEncryptedTokens(swEncryptedTokens.getValue());

    tokens.push_back(ets);
    tokens.push_back(ets);

    iupayload.setEdgeTokenSet(tokens);

    FLE2IndexedRangeEncryptedValue serverPayload(iupayload, {123456, 123456, 123456});

    auto swBuf = serverPayload.serialize(serverEncryptToken);
    ASSERT_OK(swBuf.getStatus());

    auto swServerPayload =
        FLE2IndexedRangeEncryptedValue::decryptAndParse(serverEncryptToken, swBuf.getValue());

    ASSERT_OK(swServerPayload.getStatus());
    auto sp = swServerPayload.getValue();
    ASSERT_EQ(sp.tokens.size(), 3);
    for (size_t i = 0; i < sp.tokens.size(); i++) {
        auto ets = sp.tokens[i];
        auto rhs = serverPayload.tokens[i];
        ASSERT_EQ(ets.edc, rhs.edc);
        ASSERT_EQ(ets.esc, rhs.esc);
        ASSERT_EQ(ets.ecc, rhs.ecc);
        ASSERT_EQ(sp.counters[i], serverPayload.counters[i]);
    }

    ASSERT(sp.clientEncryptedValue == serverPayload.clientEncryptedValue);
    ASSERT_EQ(serverPayload.clientEncryptedValue.size(), value.length());
    ASSERT(std::equal(serverPayload.clientEncryptedValue.begin(),
                      serverPayload.clientEncryptedValue.end(),
                      value.data<uint8_t>()));
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

    return EncryptedFieldConfig::parse(IDLParserContext("root"), fromjson(schema));
}

TEST(EncryptionInformation, RoundTrip) {
    NamespaceString ns("test.test");

    EncryptedFieldConfig efc = getTestEncryptedFieldConfig();
    auto obj = EncryptionInformationHelpers::encryptionInformationSerialize(ns, efc);


    EncryptedFieldConfig efc2 = EncryptionInformationHelpers::getAndValidateSchema(
        ns, EncryptionInformation::parse(IDLParserContext("foo"), obj));

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
                           ns, EncryptionInformation::parse(IDLParserContext("foo"), obj)),
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
                               ns, EncryptionInformation::parse(IDLParserContext("foo"), obj)),
                           DBException,
                           6371207);
    }
    {
        EncryptedFieldConfig efc = getTestEncryptedFieldConfig();
        efc.setEccCollection(boost::none);
        auto obj = EncryptionInformationHelpers::encryptionInformationSerialize(ns, efc);
        ASSERT_THROWS_CODE(EncryptionInformationHelpers::getAndValidateSchema(
                               ns, EncryptionInformation::parse(IDLParserContext("foo"), obj)),
                           DBException,
                           6371206);
    }
    {
        EncryptedFieldConfig efc = getTestEncryptedFieldConfig();
        efc.setEcocCollection(boost::none);
        auto obj = EncryptionInformationHelpers::encryptionInformationSerialize(ns, efc);
        ASSERT_THROWS_CODE(EncryptionInformationHelpers::getAndValidateSchema(
                               ns, EncryptionInformation::parse(IDLParserContext("foo"), obj)),
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
        ns, EncryptionInformation::parse(IDLParserContext("foo"), obj));

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
        if (payload.payload.getEdgeTokenSet().has_value()) {
            for (size_t i = 0; i < payload.payload.getEdgeTokenSet()->size(); i++) {
                payload.counts.push_back(1);
            }
        }

        payload.counts.push_back(1);
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
        ns, EncryptionInformation::parse(IDLParserContext("foo"), obj));

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

TEST(EDCServerCollectionTest, GenerateEDCTokens) {

    auto doc = BSON("sample" << 123456);
    auto element = doc.firstElement();

    auto value = ConstDataRange(element.value(), element.value() + element.valuesize());

    auto collectionToken = FLELevel1TokenGenerator::generateCollectionsLevel1Token(getIndexKey());
    auto edcToken = FLECollectionTokenGenerator::generateEDCToken(collectionToken);

    EDCDerivedFromDataToken edcDatakey =
        FLEDerivedFromDataTokenGenerator::generateEDCDerivedFromDataToken(edcToken, value);


    ASSERT_EQ(EDCServerCollection::generateEDCTokens(edcDatakey, 0).size(), 1);
    ASSERT_EQ(EDCServerCollection::generateEDCTokens(edcDatakey, 1).size(), 2);
    ASSERT_EQ(EDCServerCollection::generateEDCTokens(edcDatakey, 2).size(), 3);
    ASSERT_EQ(EDCServerCollection::generateEDCTokens(edcDatakey, 3).size(), 4);
}

TEST(RangeTest, Int32_NoBounds) {
#define ASSERT_EI(x, y) ASSERT_EQ(getTypeInfo32((x), boost::none, boost::none).value, (y));

    ASSERT_EI(2147483647, 4294967295);

    ASSERT_EI(1, 2147483649);
    ASSERT_EI(0, 2147483648);
    ASSERT_EI(-1, 2147483647);
    ASSERT_EI(-2, 2147483646);
    ASSERT_EI(-2147483647, 1);

    // min int32_t, no equivalent in positive part of integer
    ASSERT_EI(-2147483648, 0);

#undef ASSERT_EI
}

bool operator==(const OSTType_Int32& lhs, const OSTType_Int32& rhs) {
    return std::tie(lhs.value, lhs.min, lhs.max) == std::tie(rhs.value, rhs.min, rhs.max);
}

std::basic_ostream<char>& operator<<(std::basic_ostream<char>& os, const OSTType_Int32& lhs) {
    return os << "(" << lhs.value << ", " << lhs.min << ", " << lhs.max << ")";
}

TEST(RangeTest, Int32_Bounds) {
#define ASSERT_EIB(x, y, z, e)                   \
    {                                            \
        auto _ti = getTypeInfo32((x), (y), (z)); \
        ASSERT_EQ(_ti, (e));                     \
    }

    ASSERT_EIB(1, 1, 3, OSTType_Int32(0, 0, 2));
    ASSERT_EIB(0, 0, 1, OSTType_Int32(0, 0, 1));
    ASSERT_EIB(-1, -1, 0, OSTType_Int32(0, 0, 1));
    ASSERT_EIB(-2, -2, 0, OSTType_Int32(0, 0, 2));

    // min int32_t, no equivalent in positive part of integer
    ASSERT_EIB(-2147483647, -2147483648, 1, OSTType_Int32(1, 0, 2147483649));
    ASSERT_EIB(-2147483648, -2147483648, 0, OSTType_Int32(0, 0, 2147483648));
    ASSERT_EIB(0, -2147483648, 1, OSTType_Int32(2147483648, 0, 2147483649));
    ASSERT_EIB(1, -2147483648, 2, OSTType_Int32(2147483649, 0, 2147483650));

    ASSERT_EIB(2147483647, -2147483647, 2147483647, OSTType_Int32(4294967294, 0, 4294967294));
    ASSERT_EIB(2147483647, -2147483648, 2147483647, OSTType_Int32(4294967295, 0, 4294967295));

    ASSERT_EIB(15, 10, 26, OSTType_Int32(5, 0, 16));

    ASSERT_EIB(15, -10, 55, OSTType_Int32(25, 0, 65));

#undef ASSERT_EIB
}

TEST(RangeTest, Int32_Errors) {
    ASSERT_THROWS_CODE(getTypeInfo32(1, boost::none, 2), AssertionException, 6775001);
    ASSERT_THROWS_CODE(getTypeInfo32(1, 0, boost::none), AssertionException, 6775001);
    ASSERT_THROWS_CODE(getTypeInfo32(1, 2, 1), AssertionException, 6775002);

    ASSERT_THROWS_CODE(getTypeInfo32(1, 2, 3), AssertionException, 6775003);
    ASSERT_THROWS_CODE(getTypeInfo32(4, 2, 3), AssertionException, 6775003);

    ASSERT_THROWS_CODE(getTypeInfo32(4, -2147483648, -2147483648), AssertionException, 6775002);
}


TEST(RangeTest, Int64_NoBounds) {
#define ASSERT_EI(x, y) ASSERT_EQ(getTypeInfo64((x), boost::none, boost::none).value, (y));

    ASSERT_EI(9223372036854775807LL, 18446744073709551615ULL);

    ASSERT_EI(1, 9223372036854775809ULL);
    ASSERT_EI(0, 9223372036854775808ULL);
    ASSERT_EI(-1, 9223372036854775807ULL);
    ASSERT_EI(-2, 9223372036854775806ULL);
    ASSERT_EI(-9223372036854775807LL, 1);

    // min Int64_t, no equivalent in positive part of integer
    ASSERT_EI(LLONG_MIN, 0);

#undef ASSERT_EI
}

bool operator==(const OSTType_Int64& lhs, const OSTType_Int64& rhs) {
    return std::tie(lhs.value, lhs.min, lhs.max) == std::tie(rhs.value, rhs.min, rhs.max);
}

std::basic_ostream<char>& operator<<(std::basic_ostream<char>& os, const OSTType_Int64& lhs) {
    return os << "(" << lhs.value << ", " << lhs.min << ", " << lhs.max << ")";
}


TEST(RangeTest, Int64_Bounds) {
#define ASSERT_EIB(x, y, z, e)                   \
    {                                            \
        auto _ti = getTypeInfo64((x), (y), (z)); \
        ASSERT_EQ(_ti, (e));                     \
    }

    ASSERT_EIB(1, 1, 2, OSTType_Int64(0, 0, 1));
    ASSERT_EIB(0, 0, 1, OSTType_Int64(0, 0, 1))
    ASSERT_EIB(-1, -1, 0, OSTType_Int64(0, 0, 1))
    ASSERT_EIB(-2, -2, 0, OSTType_Int64(0, 0, 2))

    // min Int64_t, no equivalent in positive part of integer
    ASSERT_EIB(-9223372036854775807LL, LLONG_MIN, 1, OSTType_Int64(1, 0, 9223372036854775809ULL));
    ASSERT_EIB(LLONG_MIN, LLONG_MIN, 0, OSTType_Int64(0, 0, 9223372036854775808ULL));
    ASSERT_EIB(0, LLONG_MIN, 37, OSTType_Int64(9223372036854775808ULL, 0, 9223372036854775845ULL));
    ASSERT_EIB(1, LLONG_MIN, 42, OSTType_Int64(9223372036854775809ULL, 0, 9223372036854775850ULL));

    ASSERT_EIB(9223372036854775807,
               -9223372036854775807,
               9223372036854775807,
               OSTType_Int64(18446744073709551614ULL, 0, 18446744073709551614ULL));
    ASSERT_EIB(9223372036854775807,
               LLONG_MIN,
               9223372036854775807,
               OSTType_Int64(18446744073709551615ULL, 0, 18446744073709551615ULL));

    ASSERT_EIB(15, 10, 26, OSTType_Int64(5, 0, 16));

    ASSERT_EIB(15, -10, 55, OSTType_Int64(25, 0, 65));

#undef ASSERT_EIB
}

TEST(RangeTest, Int64_Errors) {
    ASSERT_THROWS_CODE(getTypeInfo64(1, boost::none, 2), AssertionException, 6775004);
    ASSERT_THROWS_CODE(getTypeInfo64(1, 0, boost::none), AssertionException, 6775004);
    ASSERT_THROWS_CODE(getTypeInfo64(1, 2, 1), AssertionException, 6775005);

    ASSERT_THROWS_CODE(getTypeInfo64(1, 2, 3), AssertionException, 6775006);
    ASSERT_THROWS_CODE(getTypeInfo64(4, 2, 3), AssertionException, 6775006);

    ASSERT_THROWS_CODE(getTypeInfo64(4, LLONG_MIN, LLONG_MIN), AssertionException, 6775005);
}


TEST(RangeTest, Double_Bounds) {
#define ASSERT_EIB(x, z) ASSERT_EQ(getTypeInfoDouble((x), -1E100, 1E100).value, (z));

    // Larger numbers map to larger uint64
    ASSERT_EIB(-1111, 4570770991734587392ULL);
    ASSERT_EIB(-111, 4585860689314185216ULL);
    ASSERT_EIB(-11, 4600989969312382976ULL);
    ASSERT_EIB(-10, 4601552919265804288ULL);
    ASSERT_EIB(-3, 4609434218613702656ULL);
    ASSERT_EIB(-2, 4611686018427387904ULL);

    ASSERT_EIB(-1, 4616189618054758400ULL);
    ASSERT_EIB(1, 13830554455654793216ULL);
    ASSERT_EIB(22, 13850257704024539136ULL);
    ASSERT_EIB(333, 13867937850999177216ULL);

    // Larger exponents map to larger uint64
    ASSERT_EIB(33E56, 14690973652625833878ULL);
    ASSERT_EIB(22E57, 14703137697061005818ULL);
    ASSERT_EIB(11E58, 14713688953586463292ULL);

    // Smaller exponents map to smaller uint64
    ASSERT_EIB(1E-6, 13740701229962882445ULL);
    ASSERT_EIB(1E-7, 13725520251343122248ULL);
    ASSERT_EIB(1E-8, 13710498295186492474ULL);
    ASSERT_EIB(1E-56, 12992711961033031890ULL);
    ASSERT_EIB(1E-57, 12977434315086142017ULL);
    ASSERT_EIB(1E-58, 12962510038552207822ULL);

    // Smaller negative exponents map to smaller uint64
    ASSERT_EIB(-1E-06, 4706042843746669171ULL);
    ASSERT_EIB(-1E-07, 4721223822366429368ULL);
    ASSERT_EIB(-1E-08, 4736245778523059142ULL);
    ASSERT_EIB(-1E-56, 5454032112676519726ULL);
    ASSERT_EIB(-1E-57, 5469309758623409599ULL);
    ASSERT_EIB(-1E-58, 5484234035157343794ULL);

    // Larger exponents map to larger uint64
    ASSERT_EIB(-33E+56, 3755770421083717738ULL);
    ASSERT_EIB(-22E+57, 3743606376648545798ULL);
    ASSERT_EIB(-11E+58, 3733055120123088324ULL);

#undef ASSERT_EIB
}


TEST(RangeTest, Double_Errors) {
    ASSERT_THROWS_CODE(getTypeInfoDouble(1, boost::none, 2), AssertionException, 6775007);
    ASSERT_THROWS_CODE(getTypeInfoDouble(1, 0, boost::none), AssertionException, 6775007);
    ASSERT_THROWS_CODE(getTypeInfoDouble(1, 2, 1), AssertionException, 6775009);


    ASSERT_THROWS_CODE(getTypeInfoDouble(1, 2, 3), AssertionException, 6775010);
    ASSERT_THROWS_CODE(getTypeInfoDouble(4, 2, 3), AssertionException, 6775010);


    ASSERT_THROWS_CODE(getTypeInfoDouble(std::numeric_limits<double>::infinity(), 1, 2),
                       AssertionException,
                       6775008);
    ASSERT_THROWS_CODE(getTypeInfoDouble(std::numeric_limits<double>::quiet_NaN(), 1, 2),
                       AssertionException,
                       6775008);
    ASSERT_THROWS_CODE(getTypeInfoDouble(std::numeric_limits<double>::signaling_NaN(), 1, 2),
                       AssertionException,
                       6775008);
}

// Edge calculator

template <typename T>
struct EdgeCalcTestVector {
    std::function<std::unique_ptr<Edges>(T, boost::optional<T>, boost::optional<T>, int)> getEdgesT;
    T value;
    boost::optional<T> min, max;
    int sparsity;
    stdx::unordered_set<std::string> expectedEdges;

    bool validate() const {
        auto edgeCalc = getEdgesT(value, min, max, sparsity);
        auto edges = edgeCalc->get();
        for (const std::string& ee : expectedEdges) {
            if (!std::any_of(edges.begin(), edges.end(), [ee](auto edge) { return edge == ee; })) {
                LOGV2_ERROR(6775120,
                            "Expected edge not found",
                            "expected-edge"_attr = ee,
                            "value"_attr = value,
                            "min"_attr = min,
                            "max"_attr = max,
                            "sparsity"_attr = sparsity,
                            "edges"_attr = edges,
                            "expected-edges"_attr = expectedEdges);
                return false;
            }
        }

        for (const StringData& edgeSd : edges) {
            std::string edge = edgeSd.toString();
            if (std::all_of(expectedEdges.begin(), expectedEdges.end(), [edge](auto ee) {
                    return edge != ee;
                })) {
                LOGV2_ERROR(6775121,
                            "An edge was found that is not expected",
                            "edge"_attr = edge,
                            "value"_attr = value,
                            "min"_attr = min,
                            "max"_attr = max,
                            "sparsity"_attr = sparsity,
                            "edges"_attr = edges,
                            "expected-edges"_attr = expectedEdges);
                return false;
            }
        }

        if (edges.size() != expectedEdges.size()) {
            LOGV2_ERROR(6775122,
                        "Unexpected number of elements in edges. Check for duplicates",
                        "value"_attr = value,
                        "min"_attr = min,
                        "max"_attr = max,
                        "sparsity"_attr = sparsity,
                        "edges"_attr = edges,
                        "expected-edges"_attr = expectedEdges);
            return false;
        }

        return true;
    }
};

TEST(EdgeCalcTest, Int32_TestVectors) {
    std::vector<EdgeCalcTestVector<int32_t>> testVectors = {
#include "test_vectors/edges_int32.cstruct"
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate());
    }
}

TEST(EdgeCalcTest, Int64_TestVectors) {
    std::vector<EdgeCalcTestVector<int64_t>> testVectors = {
#include "test_vectors/edges_int64.cstruct"
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate());
    }
}

TEST(EdgeCalcTest, Double_TestVectors) {
    std::vector<EdgeCalcTestVector<double>> testVectors = {
#include "test_vectors/edges_double.cstruct"
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate());
    }
}

TEST(EdgeCalcTest, SparsityConstraints) {
    ASSERT_THROWS_CODE(getEdgesInt32(1, 0, 8, 0), AssertionException, 6775101);
    ASSERT_THROWS_CODE(getEdgesInt32(1, 0, 8, -1), AssertionException, 6775101);
    ASSERT_THROWS_CODE(getEdgesInt64(1, 0, 8, 0), AssertionException, 6775101);
    ASSERT_THROWS_CODE(getEdgesInt64(1, 0, 8, -1), AssertionException, 6775101);
    ASSERT_THROWS_CODE(getEdgesDouble(1.0, 0.0, 8.0, 0), AssertionException, 6775101);
    ASSERT_THROWS_CODE(getEdgesDouble(1.0, 0.0, 8.0, -1), AssertionException, 6775101);
}

template <typename T>
struct MinCoverTestVector {
    T rangeMin, rangeMax;
    T min, max;
    int sparsity;
    const char* expect;

    bool validate(
        std::function<std::vector<std::string>(T, T, boost::optional<T>, boost::optional<T>, int)>
            algo) const {
        auto result = algo(rangeMin, rangeMax, min, max, sparsity);

        std::stringstream ss(expect);
        std::vector<std::string> vexpect;
        std::string item;
        while (std::getline(ss, item, '\n') && !item.empty()) {
            vexpect.push_back(std::move(item));
        }

        if (std::equal(result.begin(), result.end(), vexpect.begin())) {
            return true;
        }

        LOGV2_ERROR(6860020,
                    "MinCover algorithm produced unexpected result",
                    "rangeMin"_attr = rangeMin,
                    "rangeMax"_attr = rangeMax,
                    "min"_attr = min,
                    "max"_attr = max,
                    "sparsity"_attr = sparsity,
                    "expect"_attr = expect,
                    "got"_attr = result);
        return false;
    }
};


#pragma clang optimize off

TEST(MinCoverCalcTest, Int32_TestVectors) {
    const MinCoverTestVector<int32_t> testVectors[] = {
#include "test_vectors/mincover_int32.cstruct"
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate(minCoverInt32));
    }
}

TEST(MinCoverCalcTest, Int64_TestVectors) {
    const MinCoverTestVector<int64_t> testVectors[] = {
#include "test_vectors/mincover_int64.cstruct"
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate(minCoverInt64));
    }
}

TEST(MinCoverCalcTest, Double_TestVectors) {
    MinCoverTestVector<double> testVectors[] = {
#include "test_vectors/mincover_double.cstruct"
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate(minCoverDouble));
    }
}

#pragma clang optimize on

TEST(MinCoverCalcTest, MinCoverConstraints) {
    ASSERT_THROWS_CODE(minCoverInt32(2, 1, 0, 7, 1), AssertionException, 6860001);
    ASSERT_THROWS_CODE(minCoverInt64(2, 1, 0, 7, 1), AssertionException, 6860001);
    ASSERT_THROWS_CODE(minCoverDouble(2, 1, 0, 7, 1), AssertionException, 6860001);
}


}  // namespace mongo
