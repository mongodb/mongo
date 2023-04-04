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
#include <boost/algorithm/string/replace.hpp>
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
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/config.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/fle_fields_util.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/db/matcher/schema/encrypt_schema_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/decimal128.h"
#include "mongo/rpc/object_check.h"
#include "mongo/shell/kms_gen.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"
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
    TestKeyVault() : _localKey(getLocalKey()) {}

    static SymmetricKey getLocalKey() {
        const uint8_t buf[]{0x32, 0x78, 0x34, 0x34, 0x2b, 0x78, 0x64, 0x75, 0x54, 0x61, 0x42, 0x42,
                            0x6b, 0x59, 0x31, 0x36, 0x45, 0x72, 0x35, 0x44, 0x75, 0x41, 0x44, 0x61,
                            0x67, 0x68, 0x76, 0x53, 0x34, 0x76, 0x77, 0x64, 0x6b, 0x67, 0x38, 0x74,
                            0x70, 0x50, 0x70, 0x33, 0x74, 0x7a, 0x36, 0x67, 0x56, 0x30, 0x31, 0x41,
                            0x31, 0x43, 0x77, 0x62, 0x44, 0x39, 0x69, 0x74, 0x51, 0x32, 0x48, 0x46,
                            0x44, 0x67, 0x50, 0x57, 0x4f, 0x70, 0x38, 0x65, 0x4d, 0x61, 0x43, 0x31,
                            0x4f, 0x69, 0x37, 0x36, 0x36, 0x4a, 0x7a, 0x58, 0x5a, 0x42, 0x64, 0x42,
                            0x64, 0x62, 0x64, 0x4d, 0x75, 0x72, 0x64, 0x6f, 0x6e, 0x4a, 0x31, 0x64};

        return SymmetricKey(&buf[0], sizeof(buf), 0, SymmetricKeyId("test"), 0);
    }

    KeyMaterial getKey(const UUID& uuid) override;

    BSONObj getEncryptedKey(const UUID& uuid) override;

    SymmetricKey& getKMSLocalKey() {
        return _localKey;
    }

private:
    SymmetricKey _localKey;
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

KeyStoreRecord makeKeyStoreRecord(UUID id, ConstDataRange cdr) {
    KeyStoreRecord ksr;
    ksr.set_id(id);
    auto now = Date_t::now();
    ksr.setCreationDate(now);
    ksr.setUpdateDate(now);
    ksr.setStatus(0);
    ksr.setKeyMaterial(cdr);

    LocalMasterKey mk;

    ksr.setMasterKey(mk.toBSON());
    return ksr;
}

BSONObj TestKeyVault::getEncryptedKey(const UUID& uuid) {
    auto dek = getKey(uuid);

    std::vector<std::uint8_t> ciphertext(crypto::aeadCipherOutputLength(dek->size()));

    uassertStatusOK(crypto::aeadEncryptLocalKMS(_localKey, *dek, {ciphertext}));

    return makeKeyStoreRecord(uuid, ciphertext).toBSON();
}

TEST(FLETokens, TestVectors) {
    std::vector<uint8_t> sampleValue = {0xc0, 0x7c, 0x0d, 0xf5, 0x12, 0x57, 0x94, 0x8e,
                                        0x1a, 0x0f, 0xc7, 0x0d, 0xd4, 0x56, 0x8e, 0x3a,
                                        0xf9, 0x9b, 0x23, 0xb3, 0x43, 0x4c, 0x98, 0x58,
                                        0x23, 0x7c, 0xa7, 0xdb, 0x62, 0xdb, 0x97, 0x66};
    FLECounter counter = 1234567890;

    // Level 1
    auto collectionToken = FLELevel1TokenGenerator::generateCollectionsLevel1Token(getIndexKey());
    auto serverTokenDerivationToken =
        FLELevel1TokenGenerator::generateServerTokenDerivationLevel1Token(getIndexKey());

    ASSERT_EQUALS(CollectionsLevel1Token(decodePrf(
                      "BD53ACAC665EDD01E0CA30CB648B2B8F4967544047FD4E7D12B1A9BF07339928"_sd)),
                  collectionToken);

    ASSERT_EQUALS(ServerTokenDerivationLevel1Token(decodePrf(
                      "C17FDF249DE234F9AB15CD95137EA7EC82AE4E5B51F6BFB0FC1B8FEB6800F74C"_sd)),
                  serverTokenDerivationToken);

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

    auto serverDataToken = FLEDerivedFromDataTokenGenerator::generateServerDerivedFromDataToken(
        serverTokenDerivationToken, sampleValue);
    ASSERT_EQUALS(ServerDerivedFromDataToken(decodePrf(
                      "EDBC92F3BFE4CCB3F088FED8D42379A83F26DC37F2B6D513D4F568A6F32C8C80"_sd)),
                  serverDataToken);

    // Level 3
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

    ASSERT_EQUALS(ServerCountAndContentionFactorEncryptionToken(decodePrf(
                      "2F30DBCC06B722B60BC1FF018FC28D5FAEE2F222496BE34A264EF3267E811DA0"_sd)),
                  FLEServerMetadataEncryptionTokenGenerator::
                      generateServerCountAndContentionFactorEncryptionToken(serverDataToken));

    ASSERT_EQUALS(ServerZerosEncryptionToken(decodePrf(
                      "986F23F132FF7F14F748AC69373CFC982AD0AD4BAD25BE92008B83AB43E96029"_sd)),
                  FLEServerMetadataEncryptionTokenGenerator::generateServerZerosEncryptionToken(
                      serverDataToken));

    // Level 4
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
}

TEST(FLETokens, TestVectorUnindexedValueDecryption) {
    // Unindexed field decryption
    // Encryption can not be generated using test vectors because IV is random
    TestKeyVault keyVault;

    {
        const std::string uxCiphertext = hexblob::decode(
            "10ABCDEFAB1234987612341234567890120274E15D9477DA66394DF17BBA08FBEBB76A8BAFA63E6A7E7DCDDF9415B39877CE537469BB98A6B2B57E89AAC2CBBB5D5184DDE0111CD325E409739EF1C5C53AA917149FCF2EA2F6CB6BC8E11A7783E142FECC1570448837E6A295FCE6F16730B3"_sd);
        auto [uxBsonType, uxPlaintext] =
            FLE2UnindexedEncryptedValueV2::deserialize(&keyVault, ConstDataRange(uxCiphertext));
        ASSERT_EQUALS(uxBsonType, BSONType::String);
        ASSERT_EQUALS(
            hexblob::encode(uxPlaintext.data(), uxPlaintext.size()),
            "260000004C6F7279207761732061206D6F75736520696E2061206269672062726F776E20686F75736500");
    }
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

    auto swAnchorDoc = ESCCollection::decryptAnchorDocument(escTwiceValue, doc);
    ASSERT_OK(swDoc.getStatus());
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

    {
        // Non-anchor documents don't work with decryptAnchorDocument()
        BSONObj doc = ESCCollection::generateNonAnchorDocument(escTwiceTag, 123);
        auto swDoc = ESCCollection::decryptAnchorDocument(escTwiceValue, doc);
        ASSERT_NOT_OK(swDoc.getStatus());
        ASSERT_EQ(ErrorCodes::Error::NoSuchKey, swDoc.getStatus().code());
    }

    {
        BSONObj doc =
            ESCCollection::generateAnchorDocument(escTwiceTag, escTwiceValue, 123, 456789);
        auto swDoc = ESCCollection::decryptAnchorDocument(escTwiceValue, doc);
        ASSERT_OK(swDoc.getStatus());
        ASSERT_EQ(swDoc.getValue().position, 0);
        ASSERT_EQ(swDoc.getValue().count, 456789);
    }

    {
        BSONObj doc =
            ESCCollection::generateNullAnchorDocument(escTwiceTag, escTwiceValue, 123, 456789);
        auto swDoc = ESCCollection::decryptAnchorDocument(escTwiceValue, doc);
        ASSERT_OK(swDoc.getStatus());
        ASSERT_EQ(swDoc.getValue().position, 123);
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

namespace {

std::tuple<ESCTwiceDerivedTagToken, ESCTwiceDerivedValueToken> generateEmuBinaryTokens(
    ConstDataRange value, uint64_t contention = 0) {
    auto c1 = FLELevel1TokenGenerator::generateCollectionsLevel1Token(getIndexKey());
    auto escToken = FLECollectionTokenGenerator::generateESCToken(c1);

    ESCDerivedFromDataToken escDatakey =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, value);

    auto escDerivedToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateESCDerivedFromDataTokenAndContentionFactorToken(escDatakey, contention);

    auto escTwiceTag =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escDerivedToken);
    auto escTwiceValue =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(escDerivedToken);
    return std::tie(escTwiceTag, escTwiceValue);
}

mongo::ESCCollection::EmuBinaryResult EmuBinaryV2Test(
    boost::optional<std::pair<uint64_t, uint64_t>> nullAnchor,
    uint64_t anchorStart,
    uint64_t anchorCount,
    uint64_t anchorCposStart,
    uint64_t anchorCposEnd,
    uint64_t nonAnchorStart,
    uint64_t nonAnchorCount) {

    TestDocumentCollection coll;
    ConstDataRange value(testValue);
    auto [tagToken, valueToken] = generateEmuBinaryTokens(value, 0);

    if (nullAnchor.has_value()) {
        auto nullApos = nullAnchor->first;
        auto nullCpos = nullAnchor->second;
        // insert null anchor
        auto doc =
            ESCCollection::generateNullAnchorDocument(tagToken, valueToken, nullApos, nullCpos);
        coll.insert(doc);
    }

    ASSERT_LESS_THAN_OR_EQUALS(anchorCposStart, anchorCposEnd);

    // insert regular anchors with positions between anchorStart and anchorEnd (exclusive)
    uint64_t lastAnchorCpos = anchorCposStart;
    auto anchorEnd = anchorStart + anchorCount;
    for (auto apos = anchorStart; apos < anchorEnd; apos++) {
        auto doc =
            ESCCollection::generateAnchorDocument(tagToken, valueToken, apos, lastAnchorCpos);
        coll.insert(doc);
        if (lastAnchorCpos < anchorCposEnd) {
            lastAnchorCpos++;
        }
    }

    // insert non-anchors with positions between nonAnchorStart and nonAnchorEnd (exclusive)
    uint64_t nonAnchorEnd = nonAnchorStart + nonAnchorCount;
    for (auto cpos = nonAnchorStart; cpos < nonAnchorEnd; cpos++) {
        auto doc = ESCCollection::generateNonAnchorDocument(tagToken, cpos);
        coll.insert(doc);
    }

    auto res = ESCCollection::emuBinaryV2(coll, tagToken, valueToken);

    return res;
}
}  // namespace

// Test EmuBinaryV2 on empty collection
TEST(FLE_ESC, EmuBinaryV2_Empty) {
    auto res = EmuBinaryV2Test(boost::none, 0, 0, 0, 0, 0, 0);
    ASSERT_TRUE(res.apos.has_value());
    ASSERT_EQ(res.apos.value(), 0);
    ASSERT_TRUE(res.cpos.has_value());
    ASSERT_EQ(res.cpos.value(), 0);
}

// Test EmuBinaryV2 on ESC containing non-anchors only
TEST(FLE_ESC, EmuBinaryV2_NonAnchorsOnly) {
    auto res = EmuBinaryV2Test(boost::none, 0, 0, 0, 0, 1, 5);
    ASSERT_TRUE(res.apos.has_value());
    ASSERT_EQ(res.apos.value(), 0);
    ASSERT_TRUE(res.cpos.has_value());
    ASSERT_EQ(res.cpos.value(), 5);
}

// Test EmuBinaryV2 on ESC containing non-null anchors only
TEST(FLE_ESC, EmuBinaryV2_RegularAnchorsOnly) {
    // insert anchors 1-10, with cpos all at 0
    auto res = EmuBinaryV2Test(boost::none, 1, 10, 0, 0, 0, 0);
    ASSERT_FALSE(res.cpos.has_value());
    ASSERT_TRUE(res.apos.has_value());
    ASSERT_EQ(res.apos.value(), 10);

    // insert anchors 1-17 with cpos from 31 thru 47
    res = EmuBinaryV2Test(boost::none, 1, 17, 31, 48, 0, 0);
    ASSERT_FALSE(res.cpos.has_value());
    ASSERT_TRUE(res.apos.has_value());
    ASSERT_EQ(res.apos.value(), 17);
}

// Test EmuBinaryV2 on ESC containing both non-anchors and regular (non-null) anchors only
TEST(FLE_ESC, EmuBinaryV2_NonAnchorsAndRegularAnchorsOnly) {

    // insert regular anchors 1-7, with cpos all at 0; non-anchors 1-20
    auto res = EmuBinaryV2Test(boost::none, 1, 7, 0, 0, 1, 20);
    ASSERT_TRUE(res.cpos.has_value());
    ASSERT_EQ(res.cpos.value(), 20);
    ASSERT_TRUE(res.apos.has_value());
    ASSERT_EQ(res.apos.value(), 7);

    // insert regular anchors 1-7, with cpos between 41-47; non-anchors 1-20
    res = EmuBinaryV2Test(boost::none, 1, 7, 41, 47, 1, 20);
    ASSERT_FALSE(res.cpos.has_value());
    ASSERT_TRUE(res.apos.has_value());
    ASSERT_EQ(res.apos.value(), 7);

    // insert regular anchors 1-7, with cpos between 41-47; non-anchors 30-47
    res = EmuBinaryV2Test(boost::none, 1, 7, 41, 47, 30, 18);
    ASSERT_FALSE(res.cpos.has_value());
    ASSERT_TRUE(res.apos.has_value());
    ASSERT_EQ(res.apos.value(), 7);

    // insert regular anchors 1-7, with cpos between 41-47; non-anchors 48-59
    res = EmuBinaryV2Test(boost::none, 1, 7, 41, 47, 48, 12);
    ASSERT_TRUE(res.cpos.has_value());
    ASSERT_EQ(res.cpos.value(), 59);
    ASSERT_TRUE(res.apos.has_value());
    ASSERT_EQ(res.apos.value(), 7);
}

// Test EmuBinaryV2 on ESC containing the null anchor only
TEST(FLE_ESC, EmuBinaryV2_NullAnchorOnly) {
    std::vector<std::pair<uint64_t, uint64_t>> nullAnchors = {
        {0, 0}, {0, 10}, {10, 0}, {10, 10}, {5, 10}, {10, 5}};

    for (auto& anchor : nullAnchors) {
        auto res = EmuBinaryV2Test(anchor, 0, 0, 0, 0, 0, 0);
        ASSERT_FALSE(res.apos.has_value());
        ASSERT_FALSE(res.cpos.has_value());
    }
}

// Test EmuBinaryV2 on ESC containing null anchor and non-anchors only
TEST(FLE_ESC, EmuBinaryV2_NullAnchorAndNonAnchorsOnly) {

    // insert null anchor with apos = 0, cpos = 23; non-anchors 1-20
    auto res = EmuBinaryV2Test({{0, 23}}, 0, 0, 0, 0, 1, 20);
    ASSERT_FALSE(res.apos.has_value());
    ASSERT_FALSE(res.cpos.has_value());

    // insert null anchor with apos = 0, cpos = 23; non-anchor at 23
    res = EmuBinaryV2Test({{0, 23}}, 0, 0, 0, 0, 23, 1);
    ASSERT_FALSE(res.apos.has_value());
    ASSERT_FALSE(res.cpos.has_value());

    // insert null anchor with apos = 0, cpos = 23; non-anchors 24-29
    res = EmuBinaryV2Test({{0, 23}}, 0, 0, 0, 0, 24, 6);
    ASSERT_FALSE(res.apos.has_value());
    ASSERT_TRUE(res.cpos.has_value());
    ASSERT_EQ(res.cpos.value(), 29);

    // insert null anchor with apos = 0, cpos = 0; non-anchors 1-20
    res = EmuBinaryV2Test({{0, 0}}, 0, 0, 0, 0, 1, 20);
    ASSERT_FALSE(res.apos.has_value());
    ASSERT_TRUE(res.cpos.has_value());
    ASSERT_EQ(res.cpos.value(), 20);

    // insert null anchor with apos = 10, cpos = 0; non-anchors 1-20
    res = EmuBinaryV2Test({{10, 0}}, 0, 0, 0, 0, 1, 20);
    ASSERT_FALSE(res.apos.has_value());
    ASSERT_TRUE(res.cpos.has_value());
    ASSERT_EQ(res.cpos.value(), 20);
}

// Test EmuBinaryV2 on ESC containing null and non-null anchors only
TEST(FLE_ESC, EmuBinaryV2_NullAndRegularAnchorsOnly) {

    // insert null anchor with apos = 47, cpos = 123; regular anchors 1-20
    auto res = EmuBinaryV2Test({{47, 123}}, 1, 20, 41, 60, 0, 0);
    ASSERT_FALSE(res.apos.has_value());
    ASSERT_FALSE(res.cpos.has_value());

    // insert null anchor with apos = 47, cpos = 123; regular anchors 20-47
    res = EmuBinaryV2Test({{47, 123}}, 20, 28, 40, 57, 0, 0);
    ASSERT_FALSE(res.apos.has_value());
    ASSERT_FALSE(res.cpos.has_value());

    // insert null anchor with apos = 47, cpos = 123; regular anchors 40-59
    res = EmuBinaryV2Test({{47, 123}}, 40, 20, 40, 60, 0, 0);
    ASSERT_TRUE(res.apos.has_value());
    ASSERT_EQ(res.apos.value(), 59);
    ASSERT_FALSE(res.cpos.has_value());
}

// Test EmuBinaryV2 on ESC containing all kinds of records, where the positions in the
// null anchor are ahead of all existing anchor positions.
// e.g. (null_apos > last_apos && null_cpos >= last_anchor_cpos)
TEST(FLE_ESC, EmuBinaryV2_AllRecordTypes_NullAnchorHasNewerPositions) {
    // all tests have null anchor with null_apos=40 and null_cpos=60
    auto nullAnchor = std::make_pair<uint64_t, uint64_t>(40, 60);

    // regular anchors 1-39 (cpos 12 thru 50); non-anchors 1-49
    auto res = EmuBinaryV2Test(nullAnchor, 1, 39, 12, 50, 1, 49);
    ASSERT_FALSE(res.apos.has_value());
    ASSERT_FALSE(res.cpos.has_value());

    // regular anchors 1-39 (cpos 12 thru 50); non-anchor at 50
    res = EmuBinaryV2Test(nullAnchor, 1, 39, 12, 50, 50, 1);
    ASSERT_FALSE(res.apos.has_value());
    ASSERT_FALSE(res.cpos.has_value());

    // regular anchors 1-39 (cpos 12 thru 50); non-anchors at 51-59
    res = EmuBinaryV2Test(nullAnchor, 1, 39, 12, 50, 51, 9);
    ASSERT_FALSE(res.apos.has_value());
    ASSERT_FALSE(res.cpos.has_value());

    // regular anchors 1-39 (cpos 12 thru 50); non-anchor at 60
    res = EmuBinaryV2Test(nullAnchor, 1, 39, 12, 50, 60, 1);
    ASSERT_FALSE(res.apos.has_value());
    ASSERT_FALSE(res.cpos.has_value());

    // regular anchors 1-39 (cpos 12 thru 50); non-anchors at 61-69
    res = EmuBinaryV2Test(nullAnchor, 1, 39, 12, 50, 61, 9);
    ASSERT_FALSE(res.apos.has_value());
    ASSERT_TRUE(res.cpos.has_value());
    ASSERT_EQ(res.cpos.value(), 69);

    // regular anchors 1-39 (cpos 22 thru 60); non-anchors at 50-60
    res = EmuBinaryV2Test(nullAnchor, 1, 39, 22, 60, 50, 11);
    ASSERT_FALSE(res.apos.has_value());
    ASSERT_FALSE(res.cpos.has_value());

    // regular anchors 1-39 (cpos 22 thru 60); non-anchors at 50-69
    res = EmuBinaryV2Test(nullAnchor, 1, 39, 22, 60, 50, 20);
    ASSERT_FALSE(res.apos.has_value());
    ASSERT_TRUE(res.cpos.has_value());
    ASSERT_EQ(res.cpos.value(), 69);
}

// Test EmuBinaryV2 on ESC containing all kinds of records, where the positions in the
// null anchor are similar to the most recent regular anchor's positions.
// e.g. (null_apos == last_apos && null_cpos == last_anchor_cpos)
TEST(FLE_ESC, EmuBinaryV2_AllRecordTypes_NullAnchorHasLastAnchorPositions) {
    // all tests have null anchor with null_apos=40 and null_cpos=60
    auto nullAnchor = std::make_pair<uint64_t, uint64_t>(40, 60);

    // regular anchors 1-40 (cpos 21 thru 60); non-anchors 1-59
    auto res = EmuBinaryV2Test(nullAnchor, 1, 40, 21, 60, 1, 59);
    ASSERT_FALSE(res.apos.has_value());
    ASSERT_FALSE(res.cpos.has_value());

    // regular anchors 1-40 (cpos 21 thru 60); non-anchor at 60
    res = EmuBinaryV2Test(nullAnchor, 1, 40, 21, 60, 60, 1);
    ASSERT_FALSE(res.apos.has_value());
    ASSERT_FALSE(res.cpos.has_value());

    // regular anchors 1-40 (cpos 21 thru 60); non-anchors 61-69
    res = EmuBinaryV2Test(nullAnchor, 1, 40, 21, 60, 61, 9);
    ASSERT_FALSE(res.apos.has_value());
    ASSERT_TRUE(res.cpos.has_value());
    ASSERT_EQ(res.cpos.value(), 69);
}

// Test EmuBinaryV2 on ESC containing all kinds of records, where the positions in the null
// anchor are less than the most recent regular anchor's positions.
// e.g. (null_apos < last_apos && null_cpos <= last_anchor_cpos)
TEST(FLE_ESC, EmuBinaryV2_AllRecordTypes_NullAnchorHasOldAnchorPositions) {
    // all tests have null anchor with null_apos=40 and null_cpos=60
    auto nullAnchor = std::make_pair<uint64_t, uint64_t>(40, 60);

    // regular anchors 1-50 (cpos 11 thru 60); non-anchors 1-59
    auto res = EmuBinaryV2Test(nullAnchor, 1, 50, 11, 60, 1, 59);
    ASSERT_TRUE(res.apos.has_value());
    ASSERT_EQ(res.apos.value(), 50);
    ASSERT_FALSE(res.cpos.has_value());

    // regular anchors 1-50 (cpos 11 thru 60); non-anchor at 60
    res = EmuBinaryV2Test(nullAnchor, 1, 50, 11, 60, 60, 1);
    ASSERT_TRUE(res.apos.has_value());
    ASSERT_EQ(res.apos.value(), 50);
    ASSERT_FALSE(res.cpos.has_value());

    // regular anchors 1-50 (cpos 11 thru 60); non-anchors 61-69
    res = EmuBinaryV2Test(nullAnchor, 1, 50, 11, 60, 61, 9);
    ASSERT_TRUE(res.apos.has_value());
    ASSERT_EQ(res.apos.value(), 50);
    ASSERT_TRUE(res.cpos.has_value());
    ASSERT_EQ(res.cpos.value(), 69);

    // regular anchors 1-50 (cpos 21 thru 70); non-anchors 1-69
    res = EmuBinaryV2Test(nullAnchor, 1, 50, 21, 70, 1, 69);
    ASSERT_TRUE(res.apos.has_value());
    ASSERT_EQ(res.apos.value(), 50);
    ASSERT_FALSE(res.cpos.has_value());

    // regular anchors 1-50 (cpos 21 thru 70); non-anchor at 70
    res = EmuBinaryV2Test(nullAnchor, 1, 50, 21, 70, 70, 1);
    ASSERT_TRUE(res.apos.has_value());
    ASSERT_EQ(res.apos.value(), 50);
    ASSERT_FALSE(res.cpos.has_value());

    // regular anchors 1-50 (cpos 21 thru 70); non-anchors at 71-79
    res = EmuBinaryV2Test(nullAnchor, 1, 50, 21, 70, 71, 9);
    ASSERT_TRUE(res.apos.has_value());
    ASSERT_EQ(res.apos.value(), 50);
    ASSERT_TRUE(res.cpos.has_value());
    ASSERT_EQ(res.cpos.value(), 79);
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
    BSONObj lowerDoc, upperDoc;
    switch (value.type()) {
        case BSONType::NumberInt:
            lowerDoc = BSON("lb" << 0);
            upperDoc = BSON("ub" << 1234567);
            break;
        case BSONType::NumberLong:
            lowerDoc = BSON("lb" << 0LL);
            upperDoc = BSON("ub" << 1234567890123456789LL);
            break;
        case BSONType::NumberDouble:
            lowerDoc = BSON("lb" << 0.0);
            upperDoc = BSON("ub" << 1234567890123456789.0);
            break;
        case BSONType::Date:
            lowerDoc = BSON("lb" << Date_t::fromMillisSinceEpoch(0));
            upperDoc = BSON("ub" << Date_t::fromMillisSinceEpoch(1234567890123456789LL));
            break;
        case BSONType::NumberDecimal:
            lowerDoc = BSON("lb" << Decimal128(0));
            upperDoc = BSON("ub" << Decimal128(1234567890123456789LL));
            break;
        default:
            LOGV2_WARNING(6775520,
                          "Invalid type for range algo",
                          "algo"_attr = algorithm,
                          "type"_attr = value.type());
            lowerDoc = BSON("lb" << 0);
            upperDoc = BSON("ub" << 1234567);
            break;
    }
    insertSpec.setValue(value);
    if (value.type() == BSONType::NumberDouble || value.type() == BSONType::NumberDecimal) {
        insertSpec.setMinBound(boost::none);
        insertSpec.setMaxBound(boost::none);
    } else {
        insertSpec.setMinBound(boost::optional<IDLAnyType>(lowerDoc.firstElement()));
        insertSpec.setMaxBound(boost::optional<IDLAnyType>(upperDoc.firstElement()));
    }
    auto specDoc = BSON("s" << insertSpec.toBSON());

    FLE2RangeFindSpecEdgesInfo edgesInfo;
    FLE2RangeFindSpec findSpec;

    edgesInfo.setLowerBound(lowerDoc.firstElement());
    edgesInfo.setLbIncluded(true);
    edgesInfo.setUpperBound(upperDoc.firstElement());
    edgesInfo.setUbIncluded(true);
    edgesInfo.setIndexMin(lowerDoc.firstElement());
    edgesInfo.setIndexMax(upperDoc.firstElement());

    findSpec.setEdgesInfo(edgesInfo);

    findSpec.setFirstOperator(Fle2RangeOperator::kGt);

    findSpec.setPayloadId(1234);

    auto findDoc = BSON("s" << findSpec.toBSON());

    if (algorithm == Fle2AlgorithmInt::kRange) {
        if (operation == Operation::kFind) {
            ep.setValue(IDLAnyType(findDoc.firstElement()));
        } else if (operation == Operation::kInsert) {
            ep.setValue(IDLAnyType(specDoc.firstElement()));
        }
        ep.setSparsity(1);
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
        } else {
            payload.counts.push_back(1);
        }
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
        assertPayload(elem, EncryptedBinDataType::kFLE2FindEqualityPayloadV2);
    } else if (operation == Operation::kInsert) {
        assertPayload(elem, EncryptedBinDataType::kFLE2EqualityIndexedValueV2);
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
                          ? EncryptedBinDataType::kFLE2FindEqualityPayloadV2
                          : EncryptedBinDataType::kFLE2FindRangePayloadV2);
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
        {BSON("sample" << BSONRegEx("value1", "lu")), RegEx},
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

template <>
std::vector<uint8_t> toEncryptedVector(EncryptedBinDataType dt, ConstDataRange data) {
    std::vector<uint8_t> buf(data.length() + 1);
    buf[0] = static_cast<uint8_t>(dt);
    std::copy(data.data(), data.data() + data.length(), buf.data() + 1);
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


            auto iup = parseFromCDR<FLE2InsertUpdatePayloadV2>(subCdr);

            iup.setType(type);
            toEncryptedBinData(fieldNameToSerialize,
                               EncryptedBinDataType::kFLE2InsertUpdatePayloadV2,
                               iup,
                               builder);
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

TEST(FLE_EDC, ServerSide_Equality_Payloads_V2) {
    TestKeyVault keyVault;

    auto doc = BSON("sample" << 123456);
    auto element = doc.firstElement();

    auto value = ConstDataRange(element.value(), element.value() + element.valuesize());

    auto collectionToken = FLELevel1TokenGenerator::generateCollectionsLevel1Token(getIndexKey());
    auto serverEncryptToken =
        FLELevel1TokenGenerator::generateServerDataEncryptionLevel1Token(getIndexKey());
    auto serverDerivationToken =
        FLELevel1TokenGenerator::generateServerTokenDerivationLevel1Token(getIndexKey());

    auto edcToken = FLECollectionTokenGenerator::generateEDCToken(collectionToken);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(collectionToken);
    auto ecocToken = FLECollectionTokenGenerator::generateECOCToken(collectionToken);
    auto serverDerivedFromDataToken =
        FLEDerivedFromDataTokenGenerator::generateServerDerivedFromDataToken(serverDerivationToken,
                                                                             value);
    FLECounter counter = 0;

    EDCDerivedFromDataToken edcDatakey =
        FLEDerivedFromDataTokenGenerator::generateEDCDerivedFromDataToken(edcToken, value);
    ESCDerivedFromDataToken escDatakey =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, value);

    ESCDerivedFromDataTokenAndContentionFactorToken escDataCounterkey =
        FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
            generateESCDerivedFromDataTokenAndContentionFactorToken(escDatakey, counter);
    EDCDerivedFromDataTokenAndContentionFactorToken edcDataCounterkey =
        FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
            generateEDCDerivedFromDataTokenAndContentionFactorToken(edcDatakey, counter);

    FLE2InsertUpdatePayloadV2 iupayload;
    iupayload.setEdcDerivedToken(edcDatakey.toCDR());
    iupayload.setEscDerivedToken(escDatakey.toCDR());
    iupayload.setServerEncryptionToken(serverEncryptToken.toCDR());
    iupayload.setServerDerivedFromDataToken(serverDerivedFromDataToken.toCDR());

    auto swEncryptedTokens =
        EncryptedStateCollectionTokensV2(escDataCounterkey).serialize(ecocToken);
    uassertStatusOK(swEncryptedTokens);
    iupayload.setEncryptedTokens(swEncryptedTokens.getValue());
    iupayload.setIndexKeyId(indexKeyId);

    iupayload.setValue(value);
    iupayload.setType(element.type());

    iupayload.setContentionFactor(counter);

    auto edcTwiceDerived =
        FLETwiceDerivedTokenGenerator::generateEDCTwiceDerivedToken(edcDataCounterkey);

    auto tag = EDCServerCollection::generateTag(edcTwiceDerived, 123456);

    FLE2IndexedEqualityEncryptedValueV2 serverPayload(iupayload, tag, 123456);

    auto swBuf = serverPayload.serialize(serverEncryptToken, serverDerivedFromDataToken);
    ASSERT_OK(swBuf.getStatus());

    auto swParsedType = FLE2IndexedEqualityEncryptedValueV2::readBsonType(swBuf.getValue());
    ASSERT_OK(swParsedType.getStatus());
    ASSERT_EQ(swParsedType.getValue(), iupayload.getType());

    auto swParsedUuid = FLE2IndexedEqualityEncryptedValueV2::readKeyId(swBuf.getValue());
    ASSERT_OK(swParsedUuid.getStatus());
    ASSERT_EQ(swParsedUuid.getValue(), iupayload.getIndexKeyId());

    auto swDecryptedValue = FLE2IndexedEqualityEncryptedValueV2::parseAndDecryptCiphertext(
        serverEncryptToken, swBuf.getValue());
    ASSERT_OK(swDecryptedValue.getStatus());
    auto& clientEncryptedValue = swDecryptedValue.getValue();
    ASSERT(clientEncryptedValue == serverPayload.clientEncryptedValue);
    ASSERT_EQ(serverPayload.clientEncryptedValue.size(), value.length());
    ASSERT(std::equal(serverPayload.clientEncryptedValue.begin(),
                      serverPayload.clientEncryptedValue.end(),
                      value.data<uint8_t>()));

    auto swMetadata = FLE2IndexedEqualityEncryptedValueV2::parseAndDecryptMetadataBlock(
        serverDerivedFromDataToken, swBuf.getValue());
    ASSERT_OK(swMetadata.getStatus());
    auto& metadataBlock = swMetadata.getValue();
    ASSERT_EQ(metadataBlock.contentionFactor, counter);
    ASSERT_EQ(metadataBlock.count, 123456);
    ASSERT(metadataBlock.tag == tag);
    ASSERT_TRUE(metadataBlock.isValidZerosBlob(metadataBlock.zeros));

    auto swTag = FLE2IndexedEqualityEncryptedValueV2::parseMetadataBlockTag(swBuf.getValue());
    ASSERT_OK(swTag.getStatus());
    ASSERT_EQ(swTag.getValue(), tag);
}

TEST(FLE_EDC, ServerSide_Payloads_V2_InvalidArgs) {
    TestKeyVault keyVault;
    auto value = ConstDataRange(0, 0);
    auto bogusToken = FLELevel1TokenGenerator::generateCollectionsLevel1Token(getIndexKey());
    PrfBlock bogusTag;
    FLE2InsertUpdatePayloadV2 iupayload;

    iupayload.setValue(value);
    iupayload.setType(BSONType::NumberLong);
    iupayload.setContentionFactor(0);
    iupayload.setIndexKeyId(indexKeyId);
    iupayload.setEdcDerivedToken(bogusToken.toCDR());
    iupayload.setEscDerivedToken(bogusToken.toCDR());
    iupayload.setServerEncryptionToken(bogusToken.toCDR());
    iupayload.setServerDerivedFromDataToken(bogusToken.toCDR());
    iupayload.setEncryptedTokens(bogusToken.toCDR());

    std::vector<EdgeTokenSetV2> tokens;
    EdgeTokenSetV2 ets;
    ets.setEdcDerivedToken(bogusToken.toCDR());
    ets.setEscDerivedToken(bogusToken.toCDR());
    ets.setServerDerivedFromDataToken(bogusToken.toCDR());
    ets.setEncryptedTokens(bogusToken.toCDR());

    tokens.push_back(ets);
    tokens.push_back(ets);

    iupayload.setEdgeTokenSet(tokens);

    // Test bogus client encrypted value fails for FLE2 indexed equality value v2
    ASSERT_THROWS_CODE(
        FLE2IndexedEqualityEncryptedValueV2(iupayload, bogusTag, 0), DBException, 7290804);

    ASSERT_THROWS_CODE(
        FLE2IndexedEqualityEncryptedValueV2(BSONType::NumberLong,
                                            indexKeyId,
                                            std::vector<uint8_t>(),
                                            FLE2TagAndEncryptedMetadataBlock(0, 0, bogusTag)),
        DBException,
        7290804);

    // Test bogus client encrypted value fails for FLE2 indexed range value v2
    ASSERT_THROWS_CODE(
        FLE2IndexedRangeEncryptedValueV2(iupayload, {bogusTag}, {0}), DBException, 7290902);

    std::vector<uint8_t> arr{0x12, 0x34};
    iupayload.setValue(arr);

    iupayload.setType(100);

    // Test setting bogus type byte throws
    ASSERT_THROWS_CODE(
        FLE2IndexedEqualityEncryptedValueV2(iupayload, bogusTag, 0), DBException, 7290803);

    ASSERT_THROWS_CODE(
        FLE2IndexedRangeEncryptedValueV2(iupayload, {bogusTag}, {0}), DBException, 7290901);

    // Test mismatch vector length throws for range encrypted value v2
    ASSERT_THROWS_CODE(FLE2IndexedRangeEncryptedValueV2(iupayload, {bogusTag, bogusTag}, {0}),
                       DBException,
                       7290900);
}

TEST(FLE_EDC, ServerSide_Payloads_V2_ParseInvalidInput) {
    ConstDataRange empty(0, 0);
    auto serverToken =
        FLELevel1TokenGenerator::generateServerDataEncryptionLevel1Token(getIndexKey());
    ServerDerivedFromDataToken serverDataDerivedToken(serverToken.data);

    constexpr size_t cipherTextSize = 32;
    constexpr size_t typeOffset = UUID::kNumBytes;
    constexpr size_t edgeCountOffset = typeOffset + 1;

    std::vector<uint8_t> shortInput(edgeCountOffset + 1);
    shortInput.at(typeOffset) = static_cast<uint8_t>(BSONType::Bool);
    shortInput.at(edgeCountOffset) = 1;

    // test short input for equality payload
    ASSERT_THROWS_CODE(FLE2IndexedEqualityEncryptedValueV2::parseAndValidateFields(shortInput),
                       DBException,
                       7290802);
    ASSERT_THROWS_CODE(
        FLE2IndexedEqualityEncryptedValueV2::readKeyId(shortInput), DBException, 7290802);
    ASSERT_THROWS_CODE(
        FLE2IndexedEqualityEncryptedValueV2::readBsonType(shortInput), DBException, 7290802);
    ASSERT_THROWS_CODE(
        FLE2IndexedEqualityEncryptedValueV2::parseAndDecryptCiphertext(serverToken, shortInput),
        DBException,
        7290802);
    ASSERT_THROWS_CODE(FLE2IndexedEqualityEncryptedValueV2::parseAndDecryptMetadataBlock(
                           serverDataDerivedToken, shortInput),
                       DBException,
                       7290802);
    ASSERT_THROWS_CODE(FLE2IndexedEqualityEncryptedValueV2::parseMetadataBlockTag(shortInput),
                       DBException,
                       7290802);

    // test short input for range payload
    ASSERT_THROWS_CODE(
        FLE2IndexedRangeEncryptedValueV2::parseAndValidateFields(shortInput), DBException, 7290908);
    ASSERT_THROWS_CODE(
        FLE2IndexedRangeEncryptedValueV2::readKeyId(shortInput), DBException, 7290908);
    ASSERT_THROWS_CODE(
        FLE2IndexedRangeEncryptedValueV2::readBsonType(shortInput), DBException, 7290908);
    ASSERT_THROWS_CODE(
        FLE2IndexedRangeEncryptedValueV2::parseAndDecryptCiphertext(serverToken, shortInput),
        DBException,
        7290908);
    ASSERT_THROWS_CODE(FLE2IndexedRangeEncryptedValueV2::parseAndDecryptMetadataBlocks(
                           {serverDataDerivedToken}, shortInput),
                       DBException,
                       7290908);
    ASSERT_THROWS_CODE(
        FLE2IndexedRangeEncryptedValueV2::parseMetadataBlockTags(shortInput), DBException, 7290908);

    // test bad bson type for equality payload
    std::vector<uint8_t> badTypeInput(edgeCountOffset + 1 + cipherTextSize +
                                      sizeof(FLE2TagAndEncryptedMetadataBlock::SerializedBlob));
    badTypeInput.at(typeOffset) = 124;  // bad bsonType
    badTypeInput.at(edgeCountOffset) = 1;

    ASSERT_THROWS_CODE(FLE2IndexedEqualityEncryptedValueV2::parseAndValidateFields(badTypeInput),
                       DBException,
                       7290801);
    ASSERT_THROWS_CODE(
        FLE2IndexedEqualityEncryptedValueV2::readKeyId(badTypeInput), DBException, 7290801);
    ASSERT_THROWS_CODE(
        FLE2IndexedEqualityEncryptedValueV2::readBsonType(badTypeInput), DBException, 7290801);
    ASSERT_THROWS_CODE(
        FLE2IndexedEqualityEncryptedValueV2::parseAndDecryptCiphertext(serverToken, badTypeInput),
        DBException,
        7290801);
    ASSERT_THROWS_CODE(FLE2IndexedEqualityEncryptedValueV2::parseAndDecryptMetadataBlock(
                           serverDataDerivedToken, badTypeInput),
                       DBException,
                       7290801);
    ASSERT_THROWS_CODE(FLE2IndexedEqualityEncryptedValueV2::parseMetadataBlockTag(badTypeInput),
                       DBException,
                       7290801);

    // test bad bson type for range payload
    ASSERT_THROWS_CODE(FLE2IndexedRangeEncryptedValueV2::parseAndValidateFields(badTypeInput),
                       DBException,
                       7290906);
    ASSERT_THROWS_CODE(
        FLE2IndexedRangeEncryptedValueV2::readKeyId(badTypeInput), DBException, 7290906);
    ASSERT_THROWS_CODE(
        FLE2IndexedRangeEncryptedValueV2::readBsonType(badTypeInput), DBException, 7290906);
    ASSERT_THROWS_CODE(
        FLE2IndexedRangeEncryptedValueV2::parseAndDecryptCiphertext(serverToken, badTypeInput),
        DBException,
        7290906);
    ASSERT_THROWS_CODE(FLE2IndexedRangeEncryptedValueV2::parseAndDecryptMetadataBlocks(
                           {serverDataDerivedToken}, badTypeInput),
                       DBException,
                       7290906);
    ASSERT_THROWS_CODE(FLE2IndexedRangeEncryptedValueV2::parseMetadataBlockTags(badTypeInput),
                       DBException,
                       7290906);

    // test invalid ciphertext length for equality payload fails to decrypt
    std::vector<uint8_t> emptyEqualityCipherText(
        typeOffset + 1 + sizeof(FLE2TagAndEncryptedMetadataBlock::SerializedBlob));
    emptyEqualityCipherText.at(typeOffset) = static_cast<uint8_t>(BSONType::Bool);
    auto swDecryptedData = FLE2IndexedEqualityEncryptedValueV2::parseAndDecryptCiphertext(
        serverToken, emptyEqualityCipherText);
    ASSERT_NOT_OK(swDecryptedData.getStatus());

    // test invalid ciphertext length for range payload fails to decrypt
    std::vector<uint8_t> emptyRangeCipherText(
        edgeCountOffset + 1 + sizeof(FLE2TagAndEncryptedMetadataBlock::SerializedBlob));
    emptyRangeCipherText.at(typeOffset) = static_cast<uint8_t>(BSONType::Bool);
    emptyRangeCipherText.at(edgeCountOffset) = 1;
    swDecryptedData = FLE2IndexedRangeEncryptedValueV2::parseAndDecryptCiphertext(
        serverToken, emptyRangeCipherText);
    ASSERT_NOT_OK(swDecryptedData.getStatus());
}

TEST(FLE_EDC, ServerSide_Payloads_V2_IsValidZerosBlob) {
    FLE2TagAndEncryptedMetadataBlock::ZerosBlob zeros;
    zeros.fill(0);
    ASSERT_TRUE(FLE2TagAndEncryptedMetadataBlock::isValidZerosBlob(zeros));

    zeros[1] = 1;
    ASSERT_FALSE(FLE2TagAndEncryptedMetadataBlock::isValidZerosBlob(zeros));
}


TEST(FLE_EDC, ServerSide_Range_Payloads_V2) {
    TestKeyVault keyVault;

    auto doc = BSON("sample" << 3);
    auto element = doc.firstElement();

    auto value = ConstDataRange(element.value(), element.value() + element.valuesize());

    auto collectionToken = FLELevel1TokenGenerator::generateCollectionsLevel1Token(getIndexKey());
    auto serverEncryptToken =
        FLELevel1TokenGenerator::generateServerDataEncryptionLevel1Token(getIndexKey());
    auto serverDerivationToken =
        FLELevel1TokenGenerator::generateServerTokenDerivationLevel1Token(getIndexKey());

    auto edcToken = FLECollectionTokenGenerator::generateEDCToken(collectionToken);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(collectionToken);
    auto ecocToken = FLECollectionTokenGenerator::generateECOCToken(collectionToken);
    auto serverDerivedFromDataToken =
        FLEDerivedFromDataTokenGenerator::generateServerDerivedFromDataToken(serverDerivationToken,
                                                                             value);

    FLECounter counter = 0;

    EDCDerivedFromDataToken edcDatakey =
        FLEDerivedFromDataTokenGenerator::generateEDCDerivedFromDataToken(edcToken, value);
    ESCDerivedFromDataToken escDatakey =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, value);

    ESCDerivedFromDataTokenAndContentionFactorToken escDataCounterkey =
        FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
            generateESCDerivedFromDataTokenAndContentionFactorToken(escDatakey, counter);
    EDCDerivedFromDataTokenAndContentionFactorToken edcDataCounterkey =
        FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
            generateEDCDerivedFromDataTokenAndContentionFactorToken(edcDatakey, counter);

    FLE2InsertUpdatePayloadV2 iupayload;

    iupayload.setEdcDerivedToken(edcDatakey.toCDR());
    iupayload.setEscDerivedToken(escDatakey.toCDR());
    iupayload.setServerEncryptionToken(serverEncryptToken.toCDR());
    iupayload.setServerDerivedFromDataToken(serverDerivedFromDataToken.toCDR());

    auto swEncryptedTokens =
        EncryptedStateCollectionTokensV2(escDataCounterkey).serialize(ecocToken);
    uassertStatusOK(swEncryptedTokens);
    iupayload.setEncryptedTokens(swEncryptedTokens.getValue());
    iupayload.setIndexKeyId(indexKeyId);

    iupayload.setValue(value);
    iupayload.setType(element.type());

    iupayload.setContentionFactor(counter);

    std::vector<EdgeTokenSetV2> tokens;
    EdgeTokenSetV2 ets;
    ets.setEdcDerivedToken(edcDatakey.toCDR());
    ets.setEscDerivedToken(escDatakey.toCDR());
    ets.setServerDerivedFromDataToken(serverDerivedFromDataToken.toCDR());
    ets.setEncryptedTokens(swEncryptedTokens.getValue());

    tokens.push_back(ets);
    tokens.push_back(ets);

    iupayload.setEdgeTokenSet(tokens);

    auto edcTwiceDerived =
        FLETwiceDerivedTokenGenerator::generateEDCTwiceDerivedToken(edcDataCounterkey);

    auto tag = EDCServerCollection::generateTag(edcTwiceDerived, 123456);

    std::vector<PrfBlock> tags;
    tags.push_back(tag);
    tags.push_back(tag);

    FLE2IndexedRangeEncryptedValueV2 serverPayload(iupayload, tags, {123456, 123456});

    std::vector<ServerDerivedFromDataToken> derivedDataTokens;
    derivedDataTokens.push_back(serverDerivedFromDataToken);
    derivedDataTokens.push_back(serverDerivedFromDataToken);

    auto swBuf = serverPayload.serialize(serverEncryptToken, derivedDataTokens);
    ASSERT_OK(swBuf.getStatus());

    {
        // Test that serialize and parseAndDecryptMetadataBlocks don't work with derivedDataTokens
        // of incorrect length.
        std::vector<ServerDerivedFromDataToken> derivedDataTokensBad = derivedDataTokens;
        derivedDataTokensBad.push_back(serverDerivedFromDataToken);
        ASSERT_THROWS_CODE(serverPayload.serialize(serverEncryptToken, derivedDataTokensBad),
                           DBException,
                           7290909);

        ASSERT_THROWS_CODE(FLE2IndexedRangeEncryptedValueV2::parseAndDecryptMetadataBlocks(
                               derivedDataTokensBad, swBuf.getValue()),
                           DBException,
                           7290907);
    }

    auto swIndexKeyId = FLE2IndexedRangeEncryptedValueV2::readKeyId(swBuf.getValue());
    ASSERT_OK(swIndexKeyId.getStatus());

    auto swBsonType = FLE2IndexedRangeEncryptedValueV2::readBsonType(swBuf.getValue());
    ASSERT_OK(swBsonType.getStatus());

    auto swDecryptedValue = FLE2IndexedRangeEncryptedValueV2::parseAndDecryptCiphertext(
        serverEncryptToken, swBuf.getValue());
    ASSERT_OK(swDecryptedValue.getStatus());
    auto& clientEncryptedValue = swDecryptedValue.getValue();

    auto swBlocks = FLE2IndexedRangeEncryptedValueV2::parseAndDecryptMetadataBlocks(
        derivedDataTokens, swBuf.getValue());
    ASSERT_OK(swBlocks.getStatus());
    auto& metadataBlocks = swBlocks.getValue();

    ASSERT_EQ(swBsonType.getValue(), iupayload.getType());
    ASSERT(swIndexKeyId.getValue() == iupayload.getIndexKeyId());

    ASSERT_EQ(metadataBlocks.size(), 2);
    for (size_t i = 0; i < metadataBlocks.size(); i++) {
        ASSERT_EQ(metadataBlocks[i].contentionFactor, counter);
        ASSERT_EQ(metadataBlocks[i].count, 123456);
        ASSERT(metadataBlocks[i].tag == tag);
    }

    ASSERT(clientEncryptedValue == serverPayload.clientEncryptedValue);
    ASSERT_EQ(serverPayload.clientEncryptedValue.size(), value.length());
    ASSERT(std::equal(serverPayload.clientEncryptedValue.begin(),
                      serverPayload.clientEncryptedValue.end(),
                      value.data<uint8_t>()));

    auto swTags = FLE2IndexedRangeEncryptedValueV2::parseMetadataBlockTags(swBuf.getValue());
    ASSERT_OK(swTags.getStatus());
    ASSERT_EQ(swTags.getValue().size(), 2);
    ASSERT(swTags.getValue()[0] == tag);
    ASSERT(swTags.getValue()[1] == tag);
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

TEST(FLE_ECOC, EncryptedTokensRoundTrip) {
    std::vector<uint8_t> value(4);

    auto collectionToken = FLELevel1TokenGenerator::generateCollectionsLevel1Token(getIndexKey());
    auto escToken = FLECollectionTokenGenerator::generateESCToken(collectionToken);
    auto ecocToken = FLECollectionTokenGenerator::generateECOCToken(collectionToken);
    auto escDataToken =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, value);
    auto escContentionToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateESCDerivedFromDataTokenAndContentionFactorToken(escDataToken, 1);

    EncryptedStateCollectionTokensV2 encryptor{escContentionToken};
    auto swEncryptedTokens = encryptor.serialize(ecocToken);
    ASSERT_OK(swEncryptedTokens.getStatus());

    auto rawEcocDoc = ECOCCollection::generateDocument("foo", swEncryptedTokens.getValue());

    auto ecocDoc = ECOCCollection::parseAndDecryptV2(rawEcocDoc, ecocToken);
    ASSERT_EQ(ecocDoc.fieldName, "foo");
    ASSERT_EQ(ecocDoc.esc, escContentionToken);
}

template <typename T, typename Func>
bool vectorContains(const std::vector<T>& vec, Func func) {
    return std::find_if(vec.begin(), vec.end(), func) != vec.end();
}

EncryptedFieldConfig getTestEncryptedFieldConfig() {

    constexpr auto schema = R"({
        "escCollection": "enxcol_.coll.esc",
        "eccCollection": "enxcol_.coll.ecc",
        "ecocCollection": "enxcol_.coll.ecoc",
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
    NamespaceString ns = NamespaceString::createNamespaceString_forTest("test.test");

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

    NamespaceString ns = NamespaceString::createNamespaceString_forTest("test.test");
    ASSERT_THROWS_CODE(EncryptionInformationHelpers::getAndValidateSchema(
                           ns, EncryptionInformation::parse(IDLParserContext("foo"), obj)),
                       DBException,
                       6371205);
}

TEST(EncryptionInformation, MissingStateCollection) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest("test.test");

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
        ASSERT_DOES_NOT_THROW(EncryptionInformationHelpers::getAndValidateSchema(
            ns, EncryptionInformation::parse(IDLParserContext("foo"), obj)));
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
        auto removedFields = EDCServerCollection::getRemovedFields(origFields, origFields);
        ASSERT_EQ(removedFields.size(), 0);
    }

    {
        auto removedFields = EDCServerCollection::getRemovedFields(origFields, newFields);
        ASSERT_EQ(removedFields.size(), 0);
    }

    {
        auto removedFields = EDCServerCollection::getRemovedFields(newFields, origFields);
        ASSERT_EQ(removedFields.size(), 1);
        ASSERT_EQ(removedFields[0].fieldPathName, "c");
    }

    {
        auto removedFields = EDCServerCollection::getRemovedFields(emptyFields, origFields);
        ASSERT_EQ(removedFields.size(), 0);
    }

    {
        auto removedFields = EDCServerCollection::getRemovedFields(newFields, emptyFields);
        ASSERT_EQ(removedFields.size(), 3);
    }

    {
        auto removedFields = EDCServerCollection::getRemovedFields(newFields, newFieldsReverse);
        ASSERT_EQ(removedFields.size(), 0);
    }

    {
        auto removedFields = EDCServerCollection::getRemovedFields(origFields, origFields2);
        ASSERT_EQ(removedFields.size(), 1);
        ASSERT_EQ(removedFields[0].fieldPathName, "b");
    }


    {
        auto removedFields = EDCServerCollection::getRemovedFields(origFields, origFields2);
        ASSERT_EQ(removedFields.size(), 1);
        ASSERT_EQ(removedFields[0].fieldPathName, "b");
    }


    {
        auto removedFields = EDCServerCollection::getRemovedFields(origFields2, origFields3);
        ASSERT_EQ(removedFields.size(), 1);
        ASSERT_EQ(removedFields[0].fieldPathName, "a");
    }

    {
        auto removedFields = EDCServerCollection::getRemovedFields(origFields3, origFields3);
        ASSERT_EQ(removedFields.size(), 0);
    }

    {
        auto removedFields = EDCServerCollection::getRemovedFields(origFields3, origFields4);
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

    {
        auto blob = FLE2UnindexedEncryptedValueV2::serialize(userKey, element);
        ASSERT_EQ(blob[0], 16);

        // assert length of ciphertext (including HMAC & IV) is consistent with CBC mode
        auto cipherTextLen = blob.size() - FLE2UnindexedEncryptedValueV2::assocDataSize;
        ASSERT_EQ(cipherTextLen,
                  crypto::fle2AeadCipherOutputLength(elementData.size(), crypto::aesMode::cbc));

        auto [type, plainText] = FLE2UnindexedEncryptedValueV2::deserialize(&keyVault, {blob});
        ASSERT_EQ(type, element.type());
        ASSERT_TRUE(
            std::equal(plainText.begin(), plainText.end(), elementData.begin(), elementData.end()));
    }
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

    BSONObjBuilder sub(builder.subobjStart("nested"));
    {
        auto doc = BSON("a"
                        << "top secret");
        auto element = doc.firstElement();

        auto buf = generatePlaceholder(
            element, Operation::kInsert, Fle2AlgorithmInt::kEquality, indexKey2Id);
        builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());
    }
    {
        auto doc = BSON("a"
                        << "bottom secret");
        auto element = doc.firstElement();

        auto buf = generatePlaceholder(element, Operation::kInsert, Fle2AlgorithmInt::kUnindexed);
        builder.appendBinData("notindexed", buf.size(), BinDataType::Encrypt, buf.data());
    }
    sub.done();

    auto doc1 = builder.obj();
    auto finalDoc = encryptDocument(doc1, &keyVault, &efc);

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

TEST(FLE_Update, GetRemovedTags) {
    PrfBlock tag1 = decodePrf("BD53ACAC665EDD01E0CA30CB648B2B8F4967544047FD4E7D12B1A9BF07339928");
    PrfBlock tag2 = decodePrf("C17FDF249DE234F9AB15CD95137EA7EC82AE4E5B51F6BFB0FC1B8FEB6800F74C");
    ServerDerivedFromDataToken serverDerivedFromDataToken(
        decodePrf("986F23F132FF7F14F748AC69373CFC982AD0AD4BAD25BE92008B83AB43E96029"));
    ServerDataEncryptionLevel1Token serverToken(
        decodePrf("786F23F132FF7F14F748AC69373CFC982AD0AD4BAD25BE92008B83AB437EC82A"));

    std::vector<uint8_t> clientBlob(64);

    FLE2IndexedEqualityEncryptedValueV2 value1(
        BSONType::String, indexKeyId, clientBlob, FLE2TagAndEncryptedMetadataBlock(1, 0, tag1));
    FLE2IndexedEqualityEncryptedValueV2 value2(
        BSONType::String, indexKeyId, clientBlob, FLE2TagAndEncryptedMetadataBlock(1, 0, tag2));

    auto swValue1Blob = value1.serialize(serverToken, serverDerivedFromDataToken);
    ASSERT_OK(swValue1Blob.getStatus());
    auto swValue2Blob = value2.serialize(serverToken, serverDerivedFromDataToken);
    ASSERT_OK(swValue2Blob.getStatus());

    auto value1Blob = toEncryptedVector(EncryptedBinDataType::kFLE2EqualityIndexedValueV2,
                                        ConstDataRange(swValue1Blob.getValue()));
    auto value2Blob = toEncryptedVector(EncryptedBinDataType::kFLE2EqualityIndexedValueV2,
                                        ConstDataRange(swValue2Blob.getValue()));

    std::vector<EDCIndexedFields> oldFields = {{value1Blob, "a"}, {value2Blob, "b"}};
    std::vector<EDCIndexedFields> swappedFields = {{value2Blob, "a"}, {value1Blob, "b"}};
    std::vector<EDCIndexedFields> oneFieldChanged = {{value1Blob, "a"}, {value1Blob, "b"}};
    std::vector<EDCIndexedFields> oneFieldRemoved = {{value2Blob, "b"}};
    std::vector<EDCIndexedFields> empty;

    // Test all fields changed
    auto tagsToPull = EDCServerCollection::getRemovedTags(oldFields, swappedFields);

    ASSERT_EQ(tagsToPull.size(), 2);
    ASSERT(tagsToPull[0] == tag1 || tagsToPull[1] == tag1);
    ASSERT(tagsToPull[0] == tag2 || tagsToPull[1] == tag2);

    // Test field "b" changed
    tagsToPull = EDCServerCollection::getRemovedTags(oldFields, oneFieldChanged);
    ASSERT_EQ(tagsToPull.size(), 1);
    ASSERT(tagsToPull[0] == tag2);

    // Test all fields removed
    tagsToPull = EDCServerCollection::getRemovedTags(oldFields, empty);
    ASSERT_EQ(tagsToPull.size(), 2);
    ASSERT(tagsToPull[0] == tag1 || tagsToPull[1] == tag1);
    ASSERT(tagsToPull[0] == tag2 || tagsToPull[1] == tag2);

    // Test field "a" removed
    tagsToPull = EDCServerCollection::getRemovedTags(oldFields, oneFieldRemoved);
    ASSERT_EQ(tagsToPull.size(), 1);
    ASSERT(tagsToPull[0] == tag1);

    // Test no fields changed
    tagsToPull = EDCServerCollection::getRemovedTags(oldFields, oldFields);
    ASSERT_EQ(tagsToPull.size(), 0);

    tagsToPull = EDCServerCollection::getRemovedTags(empty, empty);
    ASSERT_EQ(tagsToPull.size(), 0);

    // Test field added
    tagsToPull = EDCServerCollection::getRemovedTags(empty, oldFields);
    ASSERT_EQ(tagsToPull.size(), 0);

    // Test exception if old fields contain deprecated FLE2 subtype...
    auto v1ValueBlob = toEncryptedVector(EncryptedBinDataType::kFLE2EqualityIndexedValue,
                                         ConstDataRange(swValue1Blob.getValue()));
    std::vector<EDCIndexedFields> v1Fields = {{v1ValueBlob, "a"}};
    ASSERT_THROWS_CODE(EDCServerCollection::getRemovedTags(v1Fields, empty), DBException, 7293204);

    // .. but not if the v1 field is also in the new document.
    tagsToPull = EDCServerCollection::getRemovedTags(v1Fields, v1Fields);
    ASSERT_EQ(tagsToPull.size(), 0);
}

TEST(FLE_Update, GenerateUpdateToRemoveTags) {
    TestKeyVault keyVault;

    auto doc = BSON("value"
                    << "123456");
    auto element = doc.firstElement();
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

    auto oldFields = EDCServerCollection::getEncryptedIndexedFields(encDoc);
    std::vector<EDCIndexedFields> newFields;

    auto removedTags = EDCServerCollection::getRemovedTags(oldFields, newFields);
    auto pullUpdate = EDCServerCollection::generateUpdateToRemoveTags(removedTags);

    std::cout << "PULL: " << pullUpdate << std::endl;

    ASSERT_EQ(pullUpdate["$pull"].type(), Object);
    ASSERT_EQ(pullUpdate["$pull"][kSafeContent].type(), Object);
    ASSERT_EQ(pullUpdate["$pull"][kSafeContent]["$in"].type(), Array);
    auto tagsArray = pullUpdate["$pull"][kSafeContent]["$in"].Array();

    ASSERT_EQ(tagsArray.size(), removedTags.size());
    for (auto& tag : tagsArray) {
        ASSERT_TRUE(tag.isBinData(BinDataType::BinDataGeneral));
    }

    // Verify failure when list of tags is empty
    ASSERT_THROWS_CODE(EDCServerCollection::generateUpdateToRemoveTags({}), DBException, 7293203);
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

TEST(EDCServerCollectionTest, ValidateModifiedDocumentCompatibility) {
    std::vector<uint8_t> blob;
    std::vector<EncryptedBinDataType> badTypes = {
        EncryptedBinDataType::kFLE2EqualityIndexedValue,
        EncryptedBinDataType::kFLE2RangeIndexedValue,
        EncryptedBinDataType::kFLE2UnindexedEncryptedValue};
    std::vector<EncryptedBinDataType> okTypes = {
        EncryptedBinDataType::kFLE2EqualityIndexedValueV2,
        EncryptedBinDataType::kFLE2RangeIndexedValueV2,
        EncryptedBinDataType::kFLE2UnindexedEncryptedValueV2};

    for (auto& badType : badTypes) {
        BSONObjBuilder builder;
        toEncryptedBinData("sample", badType, ConstDataRange(blob), &builder);
        auto doc = builder.obj();
        ASSERT_THROWS_CODE(
            EDCServerCollection::validateModifiedDocumentCompatibility(doc), DBException, 7293202);
    }

    for (auto& okType : okTypes) {
        BSONObjBuilder builder;
        toEncryptedBinData("sample", okType, ConstDataRange(blob), &builder);
        auto doc = builder.obj();
        ASSERT_DOES_NOT_THROW(EDCServerCollection::validateModifiedDocumentCompatibility(doc));
    }
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
#define ASSERT_EIB(x, z) \
    ASSERT_EQ(getTypeInfoDouble((x), boost::none, boost::none, boost::none).value, (z));

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

    ASSERT_EIB(0, 9223372036854775808ULL);
    ASSERT_EIB(-0.0, 9223372036854775808ULL);

#undef ASSERT_EIB
}

TEST(RangeTest, Double_Bounds_Precision) {
#define ASSERT_EIBP(x, y, z) ASSERT_EQ(getTypeInfoDouble((x), -100000, 100000, y).value, (z));

    ASSERT_EIBP(3.141592653589, 1, 1000031);
    ASSERT_EIBP(3.141592653589, 2, 10000314);
    ASSERT_EIBP(3.141592653589, 3, 100003141);
    ASSERT_EIBP(3.141592653589, 4, 1000031415);
    ASSERT_EIBP(3.141592653589, 5, 10000314159);
    ASSERT_EIBP(3.141592653589, 6, 100003141592);
    ASSERT_EIBP(3.141592653589, 7, 1000031415926);
#undef ASSERT_EIBP


#define ASSERT_EIBB(v, ub, lb, prc, z)                   \
    {                                                    \
        auto _ost = getTypeInfoDouble((v), lb, ub, prc); \
        ASSERT_NE(_ost.max, 18446744073709551615ULL);    \
        ASSERT_EQ(_ost.value, z);                        \
    }
#define ASSERT_EIBB_OVERFLOW(v, ub, lb, prc, z)          \
    {                                                    \
        auto _ost = getTypeInfoDouble((v), lb, ub, prc); \
        ASSERT_EQ(_ost.max, 18446744073709551615ULL);    \
        ASSERT_EQ(_ost.value, z);                        \
    }

    ASSERT_EIBB(0, 1, -1, 3, 1000);
    ASSERT_EIBB(0, 1, -1E5, 3, 100000000);

    ASSERT_EIBB(-1E-33, 1, -1E5, 3, 100000000);

    ASSERT_EIBB_OVERFLOW(0,
                         std::numeric_limits<double>::max(),
                         std::numeric_limits<double>::lowest(),
                         3,
                         9223372036854775808ULL);

    ASSERT_EIBB(3.141592653589, 5, 0, 0, 3);
    ASSERT_EIBB(3.141592653589, 5, 0, 1, 31);

    ASSERT_EIBB(3.141592653589, 5, 0, 2, 314);

    ASSERT_EIBB(3.141592653589, 5, 0, 3, 3141);
    ASSERT_EIBB(3.141592653589, 5, 0, 16, 31415926535890000);


    ASSERT_EIBB(-5, -1, -10, 3, 5000);


    ASSERT_EIBB_OVERFLOW(1E100,
                         std::numeric_limits<double>::max(),
                         std::numeric_limits<double>::lowest(),
                         3,
                         15326393489903895421ULL);

    ASSERT_EIBB(1E9, 1E10, 0, 3, 1000000000000);
    ASSERT_EIBB(1E9, 1E10, 0, 0, 1000000000);


    ASSERT_EIBB(-5, 10, -10, 0, 5);
    ASSERT_EIBB(-5, 10, -10, 2, 500);

    ASSERT_EIBB_OVERFLOW(1E-30, 10E-30, 1E-30, 35, 13381399884061196960ULL);

#undef ASSERT_EIBB
#undef ASSERT_EIBB_OVERFLOW
}

TEST(RangeTest, Double_Bounds_Precision_Errors) {

    ASSERT_THROWS_CODE(
        getTypeInfoDouble(1, boost::none, boost::none, 1), AssertionException, 6966803);

    ASSERT_THROWS_CODE(getTypeInfoDouble(1, 1, 2, -1), AssertionException, 6966801);
    ASSERT_THROWS_CODE(getTypeInfoDouble(1, 1, 2, 325), AssertionException, 6966801);
}

TEST(RangeTest, Double_Errors) {
    ASSERT_THROWS_CODE(getTypeInfoDouble(1, boost::none, 2, 5), AssertionException, 6775007);
    ASSERT_THROWS_CODE(getTypeInfoDouble(1, 0, boost::none, 5), AssertionException, 6775007);
    ASSERT_THROWS_CODE(getTypeInfoDouble(1, 2, 1, 5), AssertionException, 6775009);

    ASSERT_THROWS_CODE(getTypeInfoDouble(1, 2, 3, 5), AssertionException, 6775010);
    ASSERT_THROWS_CODE(getTypeInfoDouble(4, 2, 3, 5), AssertionException, 6775010);


    ASSERT_THROWS_CODE(getTypeInfoDouble(std::numeric_limits<double>::infinity(), 1, 2, 5),
                       AssertionException,
                       6775008);
    ASSERT_THROWS_CODE(getTypeInfoDouble(std::numeric_limits<double>::quiet_NaN(), 1, 2, 5),
                       AssertionException,
                       6775008);
    ASSERT_THROWS_CODE(getTypeInfoDouble(std::numeric_limits<double>::signaling_NaN(), 1, 2, 5),
                       AssertionException,
                       6775008);
}


TEST(EdgeCalcTest, SparsityConstraints) {
    ASSERT_THROWS_CODE(getEdgesInt32(1, 0, 8, 0), AssertionException, 6775101);
    ASSERT_THROWS_CODE(getEdgesInt32(1, 0, 8, -1), AssertionException, 6775101);
    ASSERT_THROWS_CODE(getEdgesInt64(1, 0, 8, 0), AssertionException, 6775101);
    ASSERT_THROWS_CODE(getEdgesInt64(1, 0, 8, -1), AssertionException, 6775101);
    ASSERT_THROWS_CODE(getEdgesDouble(1.0, 0.0, 8.0, 5, 0), AssertionException, 6775101);
    ASSERT_THROWS_CODE(getEdgesDouble(1.0, 0.0, 8.0, 5, -1), AssertionException, 6775101);
}

TEST(MinCoverCalcTest, MinCoverConstraints) {
    ASSERT(minCoverInt32(2, true, 1, true, 0, 7, 1).empty());
    ASSERT(minCoverInt64(2, true, 1, true, 0, 7, 1).empty());
    ASSERT(minCoverDouble(2, true, 1, true, 0, 7, boost::none, 1).empty());
    ASSERT(minCoverDecimal128(
               Decimal128(2), true, Decimal128(1), true, Decimal128(0), Decimal128(7), 5, 1)
               .empty());
}

TEST(RangeTest, Decimal128_Bounds) {
#define ASSERT_EIB(x, z)                                                                        \
    ASSERT_EQ(                                                                                  \
        boost::multiprecision::to_string(                                                       \
            getTypeInfoDecimal128(Decimal128(x), boost::none, boost::none, boost::none).value), \
        (z));

    // Larger numbers map tw larger uint64
    ASSERT_EIB(-1234567890E7, "108549948892579231731687303715884111887");
    ASSERT_EIB(-1234567890E6, "108559948892579231731687303715884111886");
    ASSERT_EIB(-1234567890E5, "108569948892579231731687303715884111885");
    ASSERT_EIB(-1234567890E4, "108579948892579231731687303715884111884");
    ASSERT_EIB(-1234567890E3, "108589948892579231731687303715884111883");
    ASSERT_EIB(-1234567890E2, "108599948892579231731687303715884111882");
    ASSERT_EIB(-1234567890E1, "108609948892579231731687303715884111881");
    ASSERT_EIB(-123456789012345, "108569948892579108281687303715884111885");
    ASSERT_EIB(-12345678901234, "108579948892579108331687303715884111884");
    ASSERT_EIB(-1234567890123, "108589948892579108731687303715884111883");
    ASSERT_EIB(-123456789012, "108599948892579111731687303715884111882");
    ASSERT_EIB(-12345678901, "108609948892579131731687303715884111881");
    ASSERT_EIB(-1234567890, "108619948892579231731687303715884111880");
    ASSERT_EIB(-99999999, "108631183460569231731687303715884111878");
    ASSERT_EIB(-8888888, "108642294572469231731687303715884111877");
    ASSERT_EIB(-777777, "108653405690469231731687303715884111876");
    ASSERT_EIB(-66666, "108664516860469231731687303715884111875");
    ASSERT_EIB(-5555, "108675628460469231731687303715884111874");
    ASSERT_EIB(-444, "108686743460469231731687303715884111873");
    ASSERT_EIB(-334, "108687843460469231731687303715884111873");
    ASSERT_EIB(-333, "108687853460469231731687303715884111873");
    ASSERT_EIB(-44, "108696783460469231731687303715884111872");
    ASSERT_EIB(-33, "108697883460469231731687303715884111872");
    ASSERT_EIB(-22, "108698983460469231731687303715884111872");
    ASSERT_EIB(-5, "108706183460469231731687303715884111871");
    ASSERT_EIB(-4, "108707183460469231731687303715884111871");
    ASSERT_EIB(-3, "108708183460469231731687303715884111871");
    ASSERT_EIB(-2, "108709183460469231731687303715884111871");
    ASSERT_EIB(-1, "108710183460469231731687303715884111871");
    ASSERT_EIB(0, "170141183460469231731687303715884105728");
    ASSERT_EIB(1, "231572183460469231731687303715884099585");
    ASSERT_EIB(2, "231573183460469231731687303715884099585");
    ASSERT_EIB(3, "231574183460469231731687303715884099585");
    ASSERT_EIB(4, "231575183460469231731687303715884099585");
    ASSERT_EIB(5, "231576183460469231731687303715884099585");
    ASSERT_EIB(22, "231583383460469231731687303715884099584");
    ASSERT_EIB(33, "231584483460469231731687303715884099584");
    ASSERT_EIB(44, "231585583460469231731687303715884099584");
    ASSERT_EIB(333, "231594513460469231731687303715884099583");
    ASSERT_EIB(334, "231594523460469231731687303715884099583");
    ASSERT_EIB(444, "231595623460469231731687303715884099583");
    ASSERT_EIB(5555, "231606738460469231731687303715884099582");
    ASSERT_EIB(66666, "231617850060469231731687303715884099581");
    ASSERT_EIB(777777, "231628961230469231731687303715884099580");
    ASSERT_EIB(8888888, "231640072348469231731687303715884099579");
    ASSERT_EIB(33E56, "232144483460469231731687303715884099528");
    ASSERT_EIB(22E57, "232153383460469231731687303715884099527");
    ASSERT_EIB(11E58, "232162283460469231731687303715884099526");

    // Smaller exponents map to smaller uint64
    ASSERT_EIB(1E-6, "231512183460469231731687303715884099591");
    ASSERT_EIB(1E-7, "231502183460469231731687303715884099592");
    ASSERT_EIB(1E-8, "231492183460469231731687303715884099593");
    ASSERT_EIB(1E-56, "231012183460469231731687303715884099641");
    ASSERT_EIB(1E-57, "231002183460469231731687303715884099642");
    ASSERT_EIB(1E-58, "230992183460469231731687303715884099643");

    // Smaller negative exponents map to smaller uint64
    ASSERT_EIB(-1E-6, "108770183460469231731687303715884111865");
    ASSERT_EIB(-1E-7, "108780183460469231731687303715884111864");
    ASSERT_EIB(-1E-8, "108790183460469231731687303715884111863");
    ASSERT_EIB(-1E-56, "109270183460469231731687303715884111815");
    ASSERT_EIB(-1E-57, "109280183460469231731687303715884111814");
    ASSERT_EIB(-1E-58, "109290183460469231731687303715884111813");

    // Larger exponents map to larger uint64
    ASSERT_EIB(-33E56, "108137883460469231731687303715884111928");
    ASSERT_EIB(-22E57, "108128983460469231731687303715884111929");
    ASSERT_EIB(-11E58, "108120083460469231731687303715884111930");

    ASSERT_EIB(Decimal128::kLargestPositive, "293021183460469231731687303715884093440");
    ASSERT_EIB(Decimal128::kSmallestPositive, "170141183460469231731687303715884105729");
    ASSERT_EIB(Decimal128::kLargestNegative, "47261183460469231731687303715884118016");
    ASSERT_EIB(Decimal128::kSmallestNegative, "170141183460469231731687303715884105727");
    ASSERT_EIB(Decimal128::kNormalizedZero, "170141183460469231731687303715884105728");
    ASSERT_EIB(Decimal128::kLargestNegativeExponentZero, "170141183460469231731687303715884105728");

#undef ASSERT_EIB
}


TEST(RangeTest, Decimal128_Bounds_Precision) {

#define ASSERT_EIBP(x, y, z)                                                                    \
    ASSERT_EQ(                                                                                  \
        getTypeInfoDecimal128(Decimal128(x), Decimal128(-100000), Decimal128(100000), y).value, \
        (z));

    ASSERT_EIBP("3.141592653589E-1", 10, 1000003141592653);
    ASSERT_EIBP("31.41592653589E-2", 10, 1000003141592653);
    ASSERT_EIBP("314.1592653589E-3", 10, 1000003141592653);
    ASSERT_EIBP("3141.592653589E-4", 10, 1000003141592653);
    ASSERT_EIBP("31415.92653589E-5", 10, 1000003141592653);
    ASSERT_EIBP("314159.2653589E-6", 10, 1000003141592653);
    ASSERT_EIBP("3141592.653589E-7", 10, 1000003141592653);
    ASSERT_EIBP("31415926.53589E-8", 10, 1000003141592653);

#undef ASSERT_EIBP

#define ASSERT_EIBPL(x, y, z)                                                                   \
    ASSERT_EQ(                                                                                  \
        getTypeInfoDecimal128(Decimal128(x), Decimal128(-100000), Decimal128("1E22"), y).value, \
        boost::multiprecision::uint128_t(z));

    ASSERT_EIBPL("3.1415926535897932384626433832795E20", 5, "31415926535897942384626433");
    ASSERT_EIBPL("3.1415926535897932384626433832795E20", 6, "314159265358979423846264338");

    ASSERT_EIBPL("3.1415926535897932384626433832795E20", 7, "3141592653589794238462643383");

    ASSERT_EIBPL("3.1415926535897932384626433832795E20", 8, "31415926535897942384626433832");

#undef ASSERT_EIBP

#define ASSERT_EIBP(x, y, z)                                                                    \
    ASSERT_EQ(                                                                                  \
        getTypeInfoDecimal128(Decimal128(x), Decimal128(-100000), Decimal128(100000), y).value, \
        (z));

    ASSERT_EIBP(3.141592653589, 1, 1000031);
    ASSERT_EIBP(3.141592653589, 2, 10000314);
    ASSERT_EIBP(3.141592653589, 3, 100003141);
    ASSERT_EIBP(3.141592653589, 4, 1000031415);
    ASSERT_EIBP(3.141592653589, 5, 10000314159);
    ASSERT_EIBP(3.141592653589, 6, 100003141592);
    ASSERT_EIBP(3.141592653589, 7, 1000031415926);
#undef ASSERT_EIBP


#define ASSERT_EIBB(v, ub, lb, prc, z)                                                         \
    {                                                                                          \
        auto _ost = getTypeInfoDecimal128(Decimal128(v), Decimal128(lb), Decimal128(ub), prc); \
        ASSERT_NE(_ost.max.str(), "340282366920938463463374607431768211455");                  \
        ASSERT_EQ(_ost.value, z);                                                              \
    }

#define ASSERT_EIBB_OVERFLOW(v, ub, lb, prc, z)                                                \
    {                                                                                          \
        auto _ost = getTypeInfoDecimal128(Decimal128(v), Decimal128(lb), Decimal128(ub), prc); \
        ASSERT_EQ(_ost.max.str(), "340282366920938463463374607431768211455");                  \
        ASSERT_EQ(_ost.value, z);                                                              \
    }

    ASSERT_EIBB(0, 1, -1, 3, 1000);
    ASSERT_EIBB(0, 1, -1E5, 3, 100000000);

    ASSERT_EIBB(-1E-33, 1, -1E5, 3, 100000000);

    ASSERT_EIBB_OVERFLOW(
        0,
        Decimal128::kLargestPositive,
        Decimal128::kLargestNegative,
        3,
        boost::multiprecision::uint128_t("170141183460469231731687303715884105728"));
    ASSERT_EIBB_OVERFLOW(
        0,
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        3,
        boost::multiprecision::uint128_t("170141183460469231731687303715884105728"));

    ASSERT_EIBB(3.141592653589, 5, 0, 0, 3);
    ASSERT_EIBB(3.141592653589, 5, 0, 1, 31);

    ASSERT_EIBB(3.141592653589, 5, 0, 2, 314);

    ASSERT_EIBB(3.141592653589, 5, 0, 3, 3141);
    ASSERT_EIBB(3.141592653589, 5, 0, 16, 31415926535890000);


    ASSERT_EIBB(-5, -1, -10, 3, 5000);


    ASSERT_EIBB_OVERFLOW(
        1E100,
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        3,
        boost::multiprecision::uint128_t("232572183460469231731687303715884099485"));

    ASSERT_EIBB(1E9, 1E10, 0, 3, 1000000000000);
    ASSERT_EIBB(1E9, 1E10, 0, 0, 1000000000);


    ASSERT_EIBB(-5, 10, -10, 0, 5);
    ASSERT_EIBB(-5, 10, -10, 2, 500);


    ASSERT_EIBB(5E-30, 10E-30, 1E-30, 35, boost::multiprecision::uint128_t("400000"));

    // Test a range that requires > 64 bits.
    ASSERT_EIBB(5, "18446744073709551616", ".1", 1, 49)
    // Test a range that requires > 64 bits.
    // min has more places after the decimal than precision.
    ASSERT_EIBB(5, "18446744073709551616", ".01", 1, 49)

#undef ASSERT_EIBB
#undef ASSERT_EIBB_OVERFLOW
}

TEST(RangeTest, Decimal128_Errors) {
    ASSERT_THROWS_CODE(getTypeInfoDecimal128(Decimal128(1), boost::none, Decimal128(2), 5),
                       AssertionException,
                       6854201);
    ASSERT_THROWS_CODE(getTypeInfoDecimal128(Decimal128(1), Decimal128(0), boost::none, 5),
                       AssertionException,
                       6854201);
    ASSERT_THROWS_CODE(getTypeInfoDecimal128(Decimal128(1), Decimal128(2), Decimal128(1), 5),
                       AssertionException,
                       6854203);


    ASSERT_THROWS_CODE(getTypeInfoDecimal128(Decimal128(1), Decimal128(2), Decimal128(3), 5),
                       AssertionException,
                       6854204);
    ASSERT_THROWS_CODE(getTypeInfoDecimal128(Decimal128(4), Decimal128(2), Decimal128(3), 5),
                       AssertionException,
                       6854204);


    ASSERT_THROWS_CODE(
        getTypeInfoDecimal128(Decimal128::kPositiveInfinity, boost::none, boost::none, boost::none),
        AssertionException,
        6854202);
    ASSERT_THROWS_CODE(
        getTypeInfoDecimal128(Decimal128::kNegativeInfinity, boost::none, boost::none, boost::none),
        AssertionException,
        6854202);

    ASSERT_THROWS_CODE(
        getTypeInfoDecimal128(Decimal128::kPositiveNaN, boost::none, boost::none, boost::none),
        AssertionException,
        6854202);

    ASSERT_THROWS_CODE(
        getTypeInfoDecimal128(Decimal128::kNegativeNaN, boost::none, boost::none, boost::none),
        AssertionException,
        6854202);
}


TEST(RangeTest, Decimal128_Bounds_Precision_Errors) {

    ASSERT_THROWS_CODE(getTypeInfoDecimal128(Decimal128(1), boost::none, boost::none, 1),
                       AssertionException,
                       6966804);

    ASSERT_THROWS_CODE(getTypeInfoDecimal128(Decimal128(1), Decimal128(1), Decimal128(2), -1),
                       AssertionException,
                       6966802);

    ASSERT_THROWS_CODE(getTypeInfoDecimal128(Decimal128(1), Decimal128(1), Decimal128(2), 6143),
                       AssertionException,
                       6966802);
}

void roundTripDecimal128_Int128(std::string dec_str) {
    Decimal128 dec(dec_str);

    auto ret = toInt128FromDecimal128(dec);

    Decimal128 roundTrip(ret.str());
    ASSERT(roundTrip == dec);
}

TEST(RangeTest, Decimal128_to_Int128) {
    roundTripDecimal128_Int128("0");
    roundTripDecimal128_Int128("123");
    roundTripDecimal128_Int128("40000000");
    roundTripDecimal128_Int128("40000000.00");
    roundTripDecimal128_Int128("40000000.00000");
    roundTripDecimal128_Int128("40000000.000000000000");

    roundTripDecimal128_Int128("40000.000E5");
    roundTripDecimal128_Int128("40000000E10");
    roundTripDecimal128_Int128("40000.000E10");
    roundTripDecimal128_Int128("40000000E20");
    roundTripDecimal128_Int128("40000.000E20");
    roundTripDecimal128_Int128("40000000E30");
    roundTripDecimal128_Int128("40000.000E30");
    roundTripDecimal128_Int128("4E37");
}

// Tests to make sure that the getMinCover() interface properly calculates the mincover when given a
// FLE2FindRangeSpec. Does not test correctness for the mincover algorithm. That testing is covered
// by the MinCoverCalcTest suite above.
template <typename A, typename B, typename C, typename D>
void assertMinCoverResult(A lb,
                          bool lbIncluded,
                          B ub,
                          bool ubIncluded,
                          C min,
                          D max,
                          int sparsity,
                          std::initializer_list<std::string> expectedList) {
    std::vector<std::string> expected{expectedList};
    std::vector<BSONElement> elems;
    auto vals = BSON_ARRAY(lb << ub << min << max);
    vals.elems(elems);

    FLE2RangeFindSpecEdgesInfo edgesInfo;

    edgesInfo.setLowerBound(elems[0]);
    edgesInfo.setLbIncluded(lbIncluded);
    edgesInfo.setUpperBound(elems[1]);
    edgesInfo.setUbIncluded(ubIncluded);
    edgesInfo.setIndexMin(elems[2]);
    edgesInfo.setIndexMax(elems[3]);

    FLE2RangeFindSpec spec;
    spec.setEdgesInfo(edgesInfo);

    spec.setFirstOperator(Fle2RangeOperator::kGt);
    spec.setPayloadId(1234);

    auto result = getMinCover(spec, sparsity);
    ASSERT_EQ(result.size(), expected.size());
    for (size_t i = 0; i < result.size(); i++) {
        ASSERT_EQ(result[i], expected[i]) << spec.toBSON();
    }
}

template <typename A, typename B, typename C, typename D>
void assertMinCoverResultPrecision(A lb,
                                   bool lbIncluded,
                                   B ub,
                                   bool ubIncluded,
                                   C min,
                                   D max,
                                   int sparsity,
                                   int precision,
                                   std::initializer_list<std::string> expectedList) {
    std::vector<std::string> expected{expectedList};
    std::vector<BSONElement> elems;
    auto vals = BSON_ARRAY(lb << ub << min << max);
    vals.elems(elems);

    FLE2RangeFindSpecEdgesInfo edgesInfo;

    edgesInfo.setLowerBound(elems[0]);
    edgesInfo.setLbIncluded(lbIncluded);
    edgesInfo.setUpperBound(elems[1]);
    edgesInfo.setUbIncluded(ubIncluded);
    edgesInfo.setIndexMin(elems[2]);
    edgesInfo.setIndexMax(elems[3]);
    edgesInfo.setPrecision(precision);

    FLE2RangeFindSpec spec;
    spec.setEdgesInfo(edgesInfo);

    spec.setFirstOperator(Fle2RangeOperator::kGt);
    spec.setPayloadId(1234);

    auto result = getMinCover(spec, sparsity);
    ASSERT_EQ(result.size(), expected.size());
    for (size_t i = 0; i < result.size(); i++) {
        ASSERT_EQ(result[i], expected[i]) << spec.toBSON();
    }
}

TEST(MinCoverInterfaceTest, Int32_Basic) {
    assertMinCoverResult(7, true, 32, true, 0, 32, 1, {"000111", "001", "01", "100000"});
    assertMinCoverResult(7, false, 32, false, 0, 32, 1, {"001", "01"});
    assertMinCoverResult(7, true, 32, false, 0, 32, 1, {"000111", "001", "01"});
    assertMinCoverResult(7, true, 32, false, 0, 32, 1, {"000111", "001", "01"});
}

TEST(MinCoverInterfaceTest, Int64_Basic) {
    assertMinCoverResult(0LL,
                         true,
                         823LL,
                         true,
                         -1000000000000000LL,
                         8070450532247928832LL,
                         2,
                         {
                             "000000000000011100011010111111010100100110001101000000",
                             "00000000000001110001101011111101010010011000110100000100",
                             "00000000000001110001101011111101010010011000110100000101",
                             "0000000000000111000110101111110101001001100011010000011000",
                             "000000000000011100011010111111010100100110001101000001100100",
                             "000000000000011100011010111111010100100110001101000001100101",
                             "000000000000011100011010111111010100100110001101000001100110",
                         });

    assertMinCoverResult(0LL,
                         false,
                         823LL,
                         false,
                         -1000000000000000LL,
                         8070450532247928832LL,
                         2,
                         {
                             "000000000000011100011010111111010100100110001101000000000000001",
                             "00000000000001110001101011111101010010011000110100000000000001",
                             "00000000000001110001101011111101010010011000110100000000000010",
                             "00000000000001110001101011111101010010011000110100000000000011",
                             "000000000000011100011010111111010100100110001101000000000001",
                             "000000000000011100011010111111010100100110001101000000000010",
                             "000000000000011100011010111111010100100110001101000000000011",
                             "0000000000000111000110101111110101001001100011010000000001",
                             "0000000000000111000110101111110101001001100011010000000010",
                             "0000000000000111000110101111110101001001100011010000000011",
                             "00000000000001110001101011111101010010011000110100000001",
                             "00000000000001110001101011111101010010011000110100000010",
                             "00000000000001110001101011111101010010011000110100000011",
                             "00000000000001110001101011111101010010011000110100000100",
                             "00000000000001110001101011111101010010011000110100000101",
                             "0000000000000111000110101111110101001001100011010000011000",
                             "000000000000011100011010111111010100100110001101000001100100",
                             "000000000000011100011010111111010100100110001101000001100101",
                             "00000000000001110001101011111101010010011000110100000110011000",
                             "00000000000001110001101011111101010010011000110100000110011001",
                             "00000000000001110001101011111101010010011000110100000110011010",
                             "000000000000011100011010111111010100100110001101000001100110110",
                         });

    assertMinCoverResult(0LL,
                         true,
                         823LL,
                         false,
                         -1000000000000000LL,
                         8070450532247928832LL,
                         2,
                         {
                             "000000000000011100011010111111010100100110001101000000",
                             "00000000000001110001101011111101010010011000110100000100",
                             "00000000000001110001101011111101010010011000110100000101",
                             "0000000000000111000110101111110101001001100011010000011000",
                             "000000000000011100011010111111010100100110001101000001100100",
                             "000000000000011100011010111111010100100110001101000001100101",
                             "00000000000001110001101011111101010010011000110100000110011000",
                             "00000000000001110001101011111101010010011000110100000110011001",
                             "00000000000001110001101011111101010010011000110100000110011010",
                             "000000000000011100011010111111010100100110001101000001100110110",
                         });

    assertMinCoverResult(0LL,
                         false,
                         823LL,
                         true,
                         -1000000000000000LL,
                         8070450532247928832LL,
                         2,
                         {
                             "000000000000011100011010111111010100100110001101000000000000001",
                             "00000000000001110001101011111101010010011000110100000000000001",
                             "00000000000001110001101011111101010010011000110100000000000010",
                             "00000000000001110001101011111101010010011000110100000000000011",
                             "000000000000011100011010111111010100100110001101000000000001",
                             "000000000000011100011010111111010100100110001101000000000010",
                             "000000000000011100011010111111010100100110001101000000000011",
                             "0000000000000111000110101111110101001001100011010000000001",
                             "0000000000000111000110101111110101001001100011010000000010",
                             "0000000000000111000110101111110101001001100011010000000011",
                             "00000000000001110001101011111101010010011000110100000001",
                             "00000000000001110001101011111101010010011000110100000010",
                             "00000000000001110001101011111101010010011000110100000011",
                             "00000000000001110001101011111101010010011000110100000100",
                             "00000000000001110001101011111101010010011000110100000101",
                             "0000000000000111000110101111110101001001100011010000011000",
                             "000000000000011100011010111111010100100110001101000001100100",
                             "000000000000011100011010111111010100100110001101000001100101",
                             "000000000000011100011010111111010100100110001101000001100110",
                         });
}

TEST(MinCoverInterfaceTest, Double_Basic) {
    assertMinCoverResult(23.5,
                         true,
                         35.25,
                         true,
                         0.0,
                         1000.0,
                         1,
                         {
                             "11000000001101111",
                             "1100000000111",
                             "1100000001000000",
                             "11000000010000010",
                             "1100000001000001100",
                             "1100000001000001101000000000000000000000000000000000000000000000",
                         });

    assertMinCoverResult(23.5,
                         false,
                         35.25,
                         false,
                         0.0,
                         1000.0,
                         1,
                         {
                             "1100000000110111100000000000000000000000000000000000000000000001",
                             "110000000011011110000000000000000000000000000000000000000000001",
                             "11000000001101111000000000000000000000000000000000000000000001",
                             "1100000000110111100000000000000000000000000000000000000000001",
                             "110000000011011110000000000000000000000000000000000000000001",
                             "11000000001101111000000000000000000000000000000000000000001",
                             "1100000000110111100000000000000000000000000000000000000001",
                             "110000000011011110000000000000000000000000000000000000001",
                             "11000000001101111000000000000000000000000000000000000001",
                             "1100000000110111100000000000000000000000000000000000001",
                             "110000000011011110000000000000000000000000000000000001",
                             "11000000001101111000000000000000000000000000000000001",
                             "1100000000110111100000000000000000000000000000000001",
                             "110000000011011110000000000000000000000000000000001",
                             "11000000001101111000000000000000000000000000000001",
                             "1100000000110111100000000000000000000000000000001",
                             "110000000011011110000000000000000000000000000001",
                             "11000000001101111000000000000000000000000000001",
                             "1100000000110111100000000000000000000000000001",
                             "110000000011011110000000000000000000000000001",
                             "11000000001101111000000000000000000000000001",
                             "1100000000110111100000000000000000000000001",
                             "110000000011011110000000000000000000000001",
                             "11000000001101111000000000000000000000001",
                             "1100000000110111100000000000000000000001",
                             "110000000011011110000000000000000000001",
                             "11000000001101111000000000000000000001",
                             "1100000000110111100000000000000000001",
                             "110000000011011110000000000000000001",
                             "11000000001101111000000000000000001",
                             "1100000000110111100000000000000001",
                             "110000000011011110000000000000001",
                             "11000000001101111000000000000001",
                             "1100000000110111100000000000001",
                             "110000000011011110000000000001",
                             "11000000001101111000000000001",
                             "1100000000110111100000000001",
                             "110000000011011110000000001",
                             "11000000001101111000000001",
                             "1100000000110111100000001",
                             "110000000011011110000001",
                             "11000000001101111000001",
                             "1100000000110111100001",
                             "110000000011011110001",
                             "11000000001101111001",
                             "1100000000110111101",
                             "110000000011011111",
                             "1100000000111",
                             "1100000001000000",
                             "11000000010000010",
                             "1100000001000001100",
                         });
    assertMinCoverResult(23.5,
                         true,
                         35.25,
                         false,
                         0.0,
                         1000.0,
                         1,
                         {
                             "11000000001101111",
                             "1100000000111",
                             "1100000001000000",
                             "11000000010000010",
                             "1100000001000001100",
                         });
    assertMinCoverResult(23.5,
                         false,
                         35.25,
                         true,
                         0.0,
                         1000.0,
                         1,
                         {
                             "1100000000110111100000000000000000000000000000000000000000000001",
                             "110000000011011110000000000000000000000000000000000000000000001",
                             "11000000001101111000000000000000000000000000000000000000000001",
                             "1100000000110111100000000000000000000000000000000000000000001",
                             "110000000011011110000000000000000000000000000000000000000001",
                             "11000000001101111000000000000000000000000000000000000000001",
                             "1100000000110111100000000000000000000000000000000000000001",
                             "110000000011011110000000000000000000000000000000000000001",
                             "11000000001101111000000000000000000000000000000000000001",
                             "1100000000110111100000000000000000000000000000000000001",
                             "110000000011011110000000000000000000000000000000000001",
                             "11000000001101111000000000000000000000000000000000001",
                             "1100000000110111100000000000000000000000000000000001",
                             "110000000011011110000000000000000000000000000000001",
                             "11000000001101111000000000000000000000000000000001",
                             "1100000000110111100000000000000000000000000000001",
                             "110000000011011110000000000000000000000000000001",
                             "11000000001101111000000000000000000000000000001",
                             "1100000000110111100000000000000000000000000001",
                             "110000000011011110000000000000000000000000001",
                             "11000000001101111000000000000000000000000001",
                             "1100000000110111100000000000000000000000001",
                             "110000000011011110000000000000000000000001",
                             "11000000001101111000000000000000000000001",
                             "1100000000110111100000000000000000000001",
                             "110000000011011110000000000000000000001",
                             "11000000001101111000000000000000000001",
                             "1100000000110111100000000000000000001",
                             "110000000011011110000000000000000001",
                             "11000000001101111000000000000000001",
                             "1100000000110111100000000000000001",
                             "110000000011011110000000000000001",
                             "11000000001101111000000000000001",
                             "1100000000110111100000000000001",
                             "110000000011011110000000000001",
                             "11000000001101111000000000001",
                             "1100000000110111100000000001",
                             "110000000011011110000000001",
                             "11000000001101111000000001",
                             "1100000000110111100000001",
                             "110000000011011110000001",
                             "11000000001101111000001",
                             "1100000000110111100001",
                             "110000000011011110001",
                             "11000000001101111001",
                             "1100000000110111101",
                             "110000000011011111",
                             "1100000000111",
                             "1100000001000000",
                             "11000000010000010",
                             "1100000001000001100",
                             "1100000001000001101000000000000000000000000000000000000000000000",
                         });
}

TEST(MinCoverInterfaceTest, Decimal_Basic) {
    assertMinCoverResult(
        Decimal128(23.5),
        true,
        Decimal128(35.25),
        true,
        Decimal128(0.0),
        Decimal128(1000.0),
        1,
        {
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "0001010111011111111111111111101",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "000101011101111111111111111111",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "00010101111",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "0001011",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "00011",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "01",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "1",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001011",
            "101011100011100101011101101111010111000001000001100001010001110000001010101110011",
            "1010111000111001010111011011110101110000010000011000010100011100000010101011101",
            "101011100011100101011101101111010111000001000001100001010001110000001010101111",
            "10101110001110010101110110111101011100000100000110000101000111000000101011",
            "101011100011100101011101101111010111000001000001100001010001110000001011",
            "1010111000111001010111011011110101110000010000011000010100011100000011",
            "10101110001110010101110110111101011100000100000110000101000111000001",
            "1010111000111001010111011011110101110000010000011000010100011100001",
            "101011100011100101011101101111010111000001000001100001010001110001",
            "10101110001110010101110110111101011100000100000110000101000111001",
            "1010111000111001010111011011110101110000010000011000010100011101",
            "101011100011100101011101101111010111000001000001100001010001111",
            "10101110001110010101110110111101011100000100000110000101001",
            "1010111000111001010111011011110101110000010000011000010101",
            "101011100011100101011101101111010111000001000001100001011",
            "1010111000111001010111011011110101110000010000011000011",
            "10101110001110010101110110111101011100000100000110001",
            "1010111000111001010111011011110101110000010000011001",
            "101011100011100101011101101111010111000001000001101",
            "10101110001110010101110110111101011100000100000111",
            "10101110001110010101110110111101011100000100001",
            "1010111000111001010111011011110101110000010001",
            "101011100011100101011101101111010111000001001",
            "10101110001110010101110110111101011100000101",
            "1010111000111001010111011011110101110000011",
            "10101110001110010101110110111101011100001",
            "1010111000111001010111011011110101110001",
            "101011100011100101011101101111010111001",
            "10101110001110010101110110111101011101",
            "1010111000111001010111011011110101111",
            "101011100011100101011101101111011",
            "1010111000111001010111011011111",
            "10101110001110010101110111",
            "10101110001110010101111",
            "1010111000111001011",
            "10101110001110011000",
            "1010111000111001100100",
            "10101110001110011001010",
            "101011100011100110010110",
            "1010111000111001100101110",
            "101011100011100110010111100",
            "10101110001110011001011110100",
            "101011100011100110010111101010",
            "10101110001110011001011110101100000000",
            "101011100011100110010111101011000000010",
            "1010111000111001100101111010110000000110000000",
            "101011100011100110010111101011000000011000000100",
            "10101110001110011001011110101100000001100000010100",
            "101011100011100110010111101011000000011000000101010000",
            "10101110001110011001011110101100000001100000010101000100",
            "1010111000111001100101111010110000000110000001010100010100000",
            "10101110001110011001011110101100000001100000010101000101000010",
            "101011100011100110010111101011000000011000000101010001010000110",
            "1010111000111001100101111010110000000110000001010100010100001110",
            "101011100011100110010111101011000000011000000101010001010000111100",
            "1010111000111001100101111010110000000110000001010100010100001111010",
            "101011100011100110010111101011000000011000000101010001010000111101100",
            "1010111000111001100101111010110000000110000001010100010100001111011010",
            "101011100011100110010111101011000000011000000101010001010000111101101100",
            "10101110001110011001011110101100000001100000010101000101000011110110110100",
            "101011100011100110010111101011000000011000000101010001010000111101101101010",
            "10101110001110011001011110101100000001100000010101000101000011110110110101100",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101000",
            "1010111000111001100101111010110000000110000001010100010100001111011011010110100100",
            "101011100011100110010111101011000000011000000101010001010000111101101101011010010100",
            "1010111000111001100101111010110000000110000001010100010100001111011011010110100101010",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "0",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "100",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000000",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000010",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011000",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110010",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011111111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111111111111111100",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111111111111111110100000000000",
        });

    assertMinCoverResult(
        Decimal128(23.5),
        false,
        Decimal128(35.25),
        false,
        Decimal128(0.0),
        Decimal128(1000.0),
        1,
        {
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "000101011101111111111111111110100000000001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "00010101110111111111111111111010000000001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "0001010111011111111111111111101000000001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "000101011101111111111111111110100000001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "00010101110111111111111111111010000001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "0001010111011111111111111111101000001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "000101011101111111111111111110100001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "00010101110111111111111111111010001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "0001010111011111111111111111101001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "000101011101111111111111111110101",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "00010101110111111111111111111011",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "000101011101111111111111111111",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "00010101111",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "0001011",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "00011",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "01",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "1",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001011",
            "101011100011100101011101101111010111000001000001100001010001110000001010101110011",
            "1010111000111001010111011011110101110000010000011000010100011100000010101011101",
            "101011100011100101011101101111010111000001000001100001010001110000001010101111",
            "10101110001110010101110110111101011100000100000110000101000111000000101011",
            "101011100011100101011101101111010111000001000001100001010001110000001011",
            "1010111000111001010111011011110101110000010000011000010100011100000011",
            "10101110001110010101110110111101011100000100000110000101000111000001",
            "1010111000111001010111011011110101110000010000011000010100011100001",
            "101011100011100101011101101111010111000001000001100001010001110001",
            "10101110001110010101110110111101011100000100000110000101000111001",
            "1010111000111001010111011011110101110000010000011000010100011101",
            "101011100011100101011101101111010111000001000001100001010001111",
            "10101110001110010101110110111101011100000100000110000101001",
            "1010111000111001010111011011110101110000010000011000010101",
            "101011100011100101011101101111010111000001000001100001011",
            "1010111000111001010111011011110101110000010000011000011",
            "10101110001110010101110110111101011100000100000110001",
            "1010111000111001010111011011110101110000010000011001",
            "101011100011100101011101101111010111000001000001101",
            "10101110001110010101110110111101011100000100000111",
            "10101110001110010101110110111101011100000100001",
            "1010111000111001010111011011110101110000010001",
            "101011100011100101011101101111010111000001001",
            "10101110001110010101110110111101011100000101",
            "1010111000111001010111011011110101110000011",
            "10101110001110010101110110111101011100001",
            "1010111000111001010111011011110101110001",
            "101011100011100101011101101111010111001",
            "10101110001110010101110110111101011101",
            "1010111000111001010111011011110101111",
            "101011100011100101011101101111011",
            "1010111000111001010111011011111",
            "10101110001110010101110111",
            "10101110001110010101111",
            "1010111000111001011",
            "10101110001110011000",
            "1010111000111001100100",
            "10101110001110011001010",
            "101011100011100110010110",
            "1010111000111001100101110",
            "101011100011100110010111100",
            "10101110001110011001011110100",
            "101011100011100110010111101010",
            "10101110001110011001011110101100000000",
            "101011100011100110010111101011000000010",
            "1010111000111001100101111010110000000110000000",
            "101011100011100110010111101011000000011000000100",
            "10101110001110011001011110101100000001100000010100",
            "101011100011100110010111101011000000011000000101010000",
            "10101110001110011001011110101100000001100000010101000100",
            "1010111000111001100101111010110000000110000001010100010100000",
            "10101110001110011001011110101100000001100000010101000101000010",
            "101011100011100110010111101011000000011000000101010001010000110",
            "1010111000111001100101111010110000000110000001010100010100001110",
            "101011100011100110010111101011000000011000000101010001010000111100",
            "1010111000111001100101111010110000000110000001010100010100001111010",
            "101011100011100110010111101011000000011000000101010001010000111101100",
            "1010111000111001100101111010110000000110000001010100010100001111011010",
            "101011100011100110010111101011000000011000000101010001010000111101101100",
            "10101110001110011001011110101100000001100000010101000101000011110110110100",
            "101011100011100110010111101011000000011000000101010001010000111101101101010",
            "10101110001110011001011110101100000001100000010101000101000011110110110101100",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101000",
            "1010111000111001100101111010110000000110000001010100010100001111011011010110100100",
            "101011100011100110010111101011000000011000000101010001010000111101101101011010010100",
            "1010111000111001100101111010110000000110000001010100010100001111011011010110100101010",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "0",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "100",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000000",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000010",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011000",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110010",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011111111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111111111111111100",
        });
    assertMinCoverResult(
        Decimal128(23.5),
        true,
        Decimal128(35.25),
        false,
        Decimal128(0.0),
        Decimal128(1000.0),
        1,
        {
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "0001010111011111111111111111101",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "000101011101111111111111111111",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "00010101111",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "0001011",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "00011",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "01",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "1",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001011",
            "101011100011100101011101101111010111000001000001100001010001110000001010101110011",
            "1010111000111001010111011011110101110000010000011000010100011100000010101011101",
            "101011100011100101011101101111010111000001000001100001010001110000001010101111",
            "10101110001110010101110110111101011100000100000110000101000111000000101011",
            "101011100011100101011101101111010111000001000001100001010001110000001011",
            "1010111000111001010111011011110101110000010000011000010100011100000011",
            "10101110001110010101110110111101011100000100000110000101000111000001",
            "1010111000111001010111011011110101110000010000011000010100011100001",
            "101011100011100101011101101111010111000001000001100001010001110001",
            "10101110001110010101110110111101011100000100000110000101000111001",
            "1010111000111001010111011011110101110000010000011000010100011101",
            "101011100011100101011101101111010111000001000001100001010001111",
            "10101110001110010101110110111101011100000100000110000101001",
            "1010111000111001010111011011110101110000010000011000010101",
            "101011100011100101011101101111010111000001000001100001011",
            "1010111000111001010111011011110101110000010000011000011",
            "10101110001110010101110110111101011100000100000110001",
            "1010111000111001010111011011110101110000010000011001",
            "101011100011100101011101101111010111000001000001101",
            "10101110001110010101110110111101011100000100000111",
            "10101110001110010101110110111101011100000100001",
            "1010111000111001010111011011110101110000010001",
            "101011100011100101011101101111010111000001001",
            "10101110001110010101110110111101011100000101",
            "1010111000111001010111011011110101110000011",
            "10101110001110010101110110111101011100001",
            "1010111000111001010111011011110101110001",
            "101011100011100101011101101111010111001",
            "10101110001110010101110110111101011101",
            "1010111000111001010111011011110101111",
            "101011100011100101011101101111011",
            "1010111000111001010111011011111",
            "10101110001110010101110111",
            "10101110001110010101111",
            "1010111000111001011",
            "10101110001110011000",
            "1010111000111001100100",
            "10101110001110011001010",
            "101011100011100110010110",
            "1010111000111001100101110",
            "101011100011100110010111100",
            "10101110001110011001011110100",
            "101011100011100110010111101010",
            "10101110001110011001011110101100000000",
            "101011100011100110010111101011000000010",
            "1010111000111001100101111010110000000110000000",
            "101011100011100110010111101011000000011000000100",
            "10101110001110011001011110101100000001100000010100",
            "101011100011100110010111101011000000011000000101010000",
            "10101110001110011001011110101100000001100000010101000100",
            "1010111000111001100101111010110000000110000001010100010100000",
            "10101110001110011001011110101100000001100000010101000101000010",
            "101011100011100110010111101011000000011000000101010001010000110",
            "1010111000111001100101111010110000000110000001010100010100001110",
            "101011100011100110010111101011000000011000000101010001010000111100",
            "1010111000111001100101111010110000000110000001010100010100001111010",
            "101011100011100110010111101011000000011000000101010001010000111101100",
            "1010111000111001100101111010110000000110000001010100010100001111011010",
            "101011100011100110010111101011000000011000000101010001010000111101101100",
            "10101110001110011001011110101100000001100000010101000101000011110110110100",
            "101011100011100110010111101011000000011000000101010001010000111101101101010",
            "10101110001110011001011110101100000001100000010101000101000011110110110101100",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101000",
            "1010111000111001100101111010110000000110000001010100010100001111011011010110100100",
            "101011100011100110010111101011000000011000000101010001010000111101101101011010010100",
            "1010111000111001100101111010110000000110000001010100010100001111011011010110100101010",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "0",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "100",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000000",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000010",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011000",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110010",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011111111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111111111111111100",
        });
    assertMinCoverResult(
        Decimal128(23.5),
        false,
        Decimal128(35.25),
        true,
        Decimal128(0.0),
        Decimal128(1000.0),
        1,
        {
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "000101011101111111111111111110100000000001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "00010101110111111111111111111010000000001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "0001010111011111111111111111101000000001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "000101011101111111111111111110100000001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "00010101110111111111111111111010000001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "0001010111011111111111111111101000001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "000101011101111111111111111110100001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "00010101110111111111111111111010001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "0001010111011111111111111111101001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "000101011101111111111111111110101",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "00010101110111111111111111111011",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "000101011101111111111111111111",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "00010101111",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "0001011",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "00011",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "001",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "01",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001010111"
            "1",
            "10101110001110010101110110111101011100000100000110000101000111000000101010111001011",
            "101011100011100101011101101111010111000001000001100001010001110000001010101110011",
            "1010111000111001010111011011110101110000010000011000010100011100000010101011101",
            "101011100011100101011101101111010111000001000001100001010001110000001010101111",
            "10101110001110010101110110111101011100000100000110000101000111000000101011",
            "101011100011100101011101101111010111000001000001100001010001110000001011",
            "1010111000111001010111011011110101110000010000011000010100011100000011",
            "10101110001110010101110110111101011100000100000110000101000111000001",
            "1010111000111001010111011011110101110000010000011000010100011100001",
            "101011100011100101011101101111010111000001000001100001010001110001",
            "10101110001110010101110110111101011100000100000110000101000111001",
            "1010111000111001010111011011110101110000010000011000010100011101",
            "101011100011100101011101101111010111000001000001100001010001111",
            "10101110001110010101110110111101011100000100000110000101001",
            "1010111000111001010111011011110101110000010000011000010101",
            "101011100011100101011101101111010111000001000001100001011",
            "1010111000111001010111011011110101110000010000011000011",
            "10101110001110010101110110111101011100000100000110001",
            "1010111000111001010111011011110101110000010000011001",
            "101011100011100101011101101111010111000001000001101",
            "10101110001110010101110110111101011100000100000111",
            "10101110001110010101110110111101011100000100001",
            "1010111000111001010111011011110101110000010001",
            "101011100011100101011101101111010111000001001",
            "10101110001110010101110110111101011100000101",
            "1010111000111001010111011011110101110000011",
            "10101110001110010101110110111101011100001",
            "1010111000111001010111011011110101110001",
            "101011100011100101011101101111010111001",
            "10101110001110010101110110111101011101",
            "1010111000111001010111011011110101111",
            "101011100011100101011101101111011",
            "1010111000111001010111011011111",
            "10101110001110010101110111",
            "10101110001110010101111",
            "1010111000111001011",
            "10101110001110011000",
            "1010111000111001100100",
            "10101110001110011001010",
            "101011100011100110010110",
            "1010111000111001100101110",
            "101011100011100110010111100",
            "10101110001110011001011110100",
            "101011100011100110010111101010",
            "10101110001110011001011110101100000000",
            "101011100011100110010111101011000000010",
            "1010111000111001100101111010110000000110000000",
            "101011100011100110010111101011000000011000000100",
            "10101110001110011001011110101100000001100000010100",
            "101011100011100110010111101011000000011000000101010000",
            "10101110001110011001011110101100000001100000010101000100",
            "1010111000111001100101111010110000000110000001010100010100000",
            "10101110001110011001011110101100000001100000010101000101000010",
            "101011100011100110010111101011000000011000000101010001010000110",
            "1010111000111001100101111010110000000110000001010100010100001110",
            "101011100011100110010111101011000000011000000101010001010000111100",
            "1010111000111001100101111010110000000110000001010100010100001111010",
            "101011100011100110010111101011000000011000000101010001010000111101100",
            "1010111000111001100101111010110000000110000001010100010100001111011010",
            "101011100011100110010111101011000000011000000101010001010000111101101100",
            "10101110001110011001011110101100000001100000010101000101000011110110110100",
            "101011100011100110010111101011000000011000000101010001010000111101101101010",
            "10101110001110011001011110101100000001100000010101000101000011110110110101100",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101000",
            "1010111000111001100101111010110000000110000001010100010100001111011011010110100100",
            "101011100011100110010111101011000000011000000101010001010000111101101101011010010100",
            "1010111000111001100101111010110000000110000001010100010100001111011011010110100101010",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "0",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "100",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000000",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000010",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011000",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110010",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "10100000110011111111111111110",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "1010000011001111111111111111100",
            "10101110001110011001011110101100000001100000010101000101000011110110110101101001010110"
            "101000001100111111111111111110100000000000",
        });
}

TEST(MinCoverInterfaceTest, InfiniteRangeBounds) {
    assertMinCoverResult(7.0,
                         true,
                         std::numeric_limits<double>::infinity(),
                         true,
                         0.0,
                         32.0,
                         1,
                         {
                             "11000000000111",
                             "11000000001",
                             "1100000001000000000000000000000000000000000000000000000000000000",
                         });
    assertMinCoverResult(0.0,
                         true,
                         8.0,
                         true,
                         0.0,
                         32.0,
                         1,
                         {
                             "10",
                             "11000000000",
                             "1100000000100000000000000000000000000000000000000000000000000000",
                         });
    assertMinCoverResult(-std::numeric_limits<double>::infinity(),
                         true,
                         8.0,
                         true,
                         0.0,
                         32.0,
                         1,
                         {
                             "10",
                             "11000000000",
                             "1100000000100000000000000000000000000000000000000000000000000000",
                         });
}

TEST(MinCoverInteraceTest, InvalidBounds) {
    assertMinCoverResult(7, true, 7, false, 0, 32, 1, {});
    assertMinCoverResult(7LL, true, 7LL, false, 0LL, 32LL, 1, {});
    assertMinCoverResult(7.0, true, 7.0, false, 0.0, 32.0, 1, {});

    assertMinCoverResult(7, false, 7, true, 0, 32, 1, {});
    assertMinCoverResult(7LL, false, 7LL, true, 0LL, 32LL, 1, {});
    assertMinCoverResult(7.0, false, 7.0, true, 0.0, 32.0, 1, {});

    ASSERT_THROWS_CODE(
        assertMinCoverResult(1, false, 1, false, 0, 1, 1, {}), AssertionException, 6901316);
    ASSERT_THROWS_CODE(
        assertMinCoverResult(0, true, 0, false, 0, 7, 1, {}), AssertionException, 6901317);
}

// Test point queries and that trimming bitstrings is correct in precision mode
TEST(MinCoverInteraceTest, Precision_Equal) {
    assertMinCoverResultPrecision(3.14159, true, 3.14159, true, 0.0, 10.0, 1, 2, {"00100111010"});
    assertMinCoverResultPrecision(Decimal128(3.14159),
                                  true,
                                  Decimal128(3.14159),
                                  true,
                                  Decimal128(0.0),
                                  Decimal128(10.0),
                                  1,
                                  2,
                                  {"00100111010"});

    assertMinCoverResultPrecision(3.1, true, 3.1, true, 0.0, 12.0, 1, 1, {"00011111"});
    assertMinCoverResultPrecision(Decimal128(3.1),
                                  true,
                                  Decimal128(3.1),
                                  true,
                                  Decimal128(0.0),
                                  Decimal128(12.0),
                                  1,
                                  1,
                                  {"00011111"});
}

DEATH_TEST_REGEX(MinCoverInterfaceTest, Error_MinMaxTypeMismatch, "Tripwire assertion.*6901300") {
    std::vector<BSONElement> elems;
    auto vals = BSON_ARRAY(10 << 11 << 4 << 11.5);
    vals.elems(elems);

    FLE2RangeFindSpecEdgesInfo edgesInfo;
    edgesInfo.setLowerBound(elems[0]);
    edgesInfo.setLbIncluded(true);
    edgesInfo.setUpperBound(elems[1]);
    edgesInfo.setUbIncluded(true);
    edgesInfo.setIndexMin(elems[2]);
    edgesInfo.setIndexMax(elems[3]);

    FLE2RangeFindSpec spec;
    spec.setEdgesInfo(edgesInfo);

    spec.setFirstOperator(Fle2RangeOperator::kGt);
    spec.setPayloadId(1234);


    getMinCover(spec, 1);
}
}  // namespace mongo
