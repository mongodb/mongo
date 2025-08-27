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

#include "mongo/crypto/fle_crypto.h"

#include "mongo/base/data_range.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/secure_allocator.h"
#include "mongo/base/status.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/crypto/aead_encryption.h"
#include "mongo/crypto/encryption_fields_validation.h"
#include "mongo/crypto/fle_data_frames.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/fle_numeric.h"
#include "mongo/crypto/fle_options_gen.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/decimal128.h"
#include "mongo/rpc/object_check.h"  // IWYU pragma: keep
#include "mongo/shell/kms_gen.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <stack>
#include <string>
#include <tuple>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/multiprecision/number.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

template <typename T>
std::string hexdump(const std::vector<T>& buf) {
    return hexdump(buf.data(), buf.size());
}

std::string hexdump(const PrfBlock& buf) {
    return hexdump(buf.data(), buf.size());
}

std::string hexdump(const ConstDataRange& buf) {
    return hexdump(buf.data(), buf.length());
}

std::vector<char> decode(StringData sd) {
    auto s = hexblob::decode(sd);
    return std::vector<char>(s.data(), s.data() + s.length());
}

PrfBlock blockToArray(StringData block) {
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

std::basic_ostream<char>& operator<<(std::basic_ostream<char>& os, const FLEToken& right) {
    return os << "{" << right.name() << ": " << hexdump(right.asPrfBlock()) << "}";
}

constexpr auto kIndexKeyId = "12345678-1234-9876-1234-123456789012"_sd;
constexpr auto kIndexKey2Id = "12345678-1234-9876-1234-123456789013"_sd;
constexpr auto kIndexKey3Id = "12345678-1234-9876-1234-123456789014"_sd;
constexpr auto kUserKeyId = "ABCDEFAB-1234-9876-1234-123456789012"_sd;
static UUID indexKeyId = uassertStatusOK(UUID::parse(kIndexKeyId));
static UUID indexKey2Id = uassertStatusOK(UUID::parse(kIndexKey2Id));
static UUID indexKey3Id = uassertStatusOK(UUID::parse(kIndexKey3Id));
static UUID userKeyId = uassertStatusOK(UUID::parse(kUserKeyId));

HmacContext hmacCtx;

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

    SymmetricKey& getKMSLocalKey() override {
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

TEST_F(ServiceContextTest, FLETokens_TestVectors) {
    std::vector<uint8_t> sampleValue = {0xc0, 0x7c, 0x0d, 0xf5, 0x12, 0x57, 0x94, 0x8e,
                                        0x1a, 0x0f, 0xc7, 0x0d, 0xd4, 0x56, 0x8e, 0x3a,
                                        0xf9, 0x9b, 0x23, 0xb3, 0x43, 0x4c, 0x98, 0x58,
                                        0x23, 0x7c, 0xa7, 0xdb, 0x62, 0xdb, 0x97, 0x66};
    FLECounter counter = 1234567890;

    // Level 1
    auto collectionToken = CollectionsLevel1Token::deriveFrom(getIndexKey());
    auto serverTokenDerivationToken = ServerTokenDerivationLevel1Token::deriveFrom(getIndexKey());

    ASSERT_EQUALS(CollectionsLevel1Token(decodePrf(
                      "BD53ACAC665EDD01E0CA30CB648B2B8F4967544047FD4E7D12B1A9BF07339928"_sd)),
                  collectionToken);

    ASSERT_EQUALS(ServerTokenDerivationLevel1Token(decodePrf(
                      "C17FDF249DE234F9AB15CD95137EA7EC82AE4E5B51F6BFB0FC1B8FEB6800F74C"_sd)),
                  serverTokenDerivationToken);

    ASSERT_EQUALS(ServerDataEncryptionLevel1Token(decodePrf(
                      "EB9A73F7912D86A4297E81D2F675AF742874E4057E3A890FEC651A23EEE3F3EC"_sd)),
                  ServerDataEncryptionLevel1Token::deriveFrom(getIndexKey()));

    // Level 2
    auto edcToken = EDCToken::deriveFrom(collectionToken);
    ASSERT_EQUALS(
        EDCToken(decodePrf("82B0AB0F8F1D31AEB6F4DBC915EF17CBA2FE21E36EC436984EB63BECEC173831"_sd)),
        edcToken);
    auto escToken = ESCToken::deriveFrom(collectionToken);
    ASSERT_EQUALS(
        ESCToken(decodePrf("279C575B52B73677EEF07D9C1126EBDF08C35369570A9B75E44A9AFDCCA96B6D"_sd)),
        escToken);
    ASSERT_EQUALS(
        ECOCToken(decodePrf("9E837ED3926CB8ED680E0E7DCB2A481A3E398BE7851FA1CE4D738FA5E67FFCC9"_sd)),
        ECOCToken::deriveFrom(collectionToken));

    auto serverDataToken =
        ServerDerivedFromDataToken::deriveFrom(serverTokenDerivationToken, sampleValue);
    ASSERT_EQUALS(ServerDerivedFromDataToken(decodePrf(
                      "EDBC92F3BFE4CCB3F088FED8D42379A83F26DC37F2B6D513D4F568A6F32C8C80"_sd)),
                  serverDataToken);

    // Level 3
    auto edcDataToken = EDCDerivedFromDataToken::deriveFrom(edcToken, sampleValue);
    ASSERT_EQUALS(EDCDerivedFromDataToken(decodePrf(
                      "CEA098AA664E578D4E9CE05B50ADD15DF2F0316CD5CCB08E720C61D8C7580E2A"_sd)),
                  edcDataToken);

    auto escDataToken = ESCDerivedFromDataToken::deriveFrom(escToken, sampleValue);
    ASSERT_EQUALS(ESCDerivedFromDataToken(decodePrf(
                      "DE6A1AC292BC62094C33E94647B044B9B10514317B75F4128DDA2E0FB686704F"_sd)),
                  escDataToken);

    ASSERT_EQUALS(ServerCountAndContentionFactorEncryptionToken(decodePrf(
                      "2F30DBCC06B722B60BC1FF018FC28D5FAEE2F222496BE34A264EF3267E811DA0"_sd)),
                  ServerCountAndContentionFactorEncryptionToken::deriveFrom(serverDataToken));

    ASSERT_EQUALS(ServerZerosEncryptionToken(decodePrf(
                      "986F23F132FF7F14F748AC69373CFC982AD0AD4BAD25BE92008B83AB43E96029"_sd)),
                  ServerZerosEncryptionToken::deriveFrom(serverDataToken));

    // Level 4
    auto edcDataCounterToken =
        EDCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(edcDataToken, counter);
    ASSERT_EQUALS(EDCDerivedFromDataTokenAndContentionFactorToken(decodePrf(
                      "D8CC38AE6A64BD1BF195A2D35734C13AF2B1729AD1052A81BE00BF29C67A696E"_sd)),
                  edcDataCounterToken);


    auto escDataCounterToken =
        ESCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(escDataToken, counter);
    ASSERT_EQUALS(ESCDerivedFromDataTokenAndContentionFactorToken(decodePrf(
                      "8AAF04CBA6DC16BFB37CADBA43DCA66C183634CB3DA278DE174556AE6E17CEBB"_sd)),
                  escDataCounterToken);

    // Level 5
    auto edcTwiceToken = EDCTwiceDerivedToken::deriveFrom(edcDataCounterToken);
    ASSERT_EQUALS(EDCTwiceDerivedToken(decodePrf(
                      "B39A7EC33FD976EFB8EEBBBF3A265A933E2128D709BB88C77E3D42AA735F697C"_sd)),
                  edcTwiceToken);

    auto escTwiceTagToken = ESCTwiceDerivedTagToken::deriveFrom(escDataCounterToken);
    ASSERT_EQUALS(ESCTwiceDerivedTagToken(decodePrf(
                      "D6F76A9D4767E0889B709517C8CF0412D81874AEB6E6CEBFBDDFF7B013EB7154"_sd)),
                  escTwiceTagToken);
    auto escTwiceValueToken = ESCTwiceDerivedValueToken::deriveFrom(escDataCounterToken);
    ASSERT_EQUALS(ESCTwiceDerivedValueToken(decodePrf(
                      "53F0A51A43447B9881D5E79BA4C5F78E80BC2BC6AA42B00C81079EBF4C9D5A7C"_sd)),
                  escTwiceValueToken);

    // Anchor Padding
    auto anchorPaddingTokenRoot = AnchorPaddingRootToken::deriveFrom(escToken);
    ASSERT_EQUALS(AnchorPaddingRootToken(decodePrf(
                      "4312890F621FE3CA7497C3405DFD8AAF46A578C77F7404D28C12BA853A4D3327"_sd)),
                  anchorPaddingTokenRoot);

    auto anchorPaddingTokenKey = AnchorPaddingKeyToken::deriveFrom(anchorPaddingTokenRoot);
    ASSERT_EQUALS(AnchorPaddingKeyToken(decodePrf(
                      "EF6D80379C462FC724CE8C245DC177ED507154B4EBB04DED780FA0DDAF1A2247"_sd)),
                  anchorPaddingTokenKey);

    auto anchorPaddingTokenValue = AnchorPaddingValueToken::deriveFrom(anchorPaddingTokenRoot);
    ASSERT_EQUALS(AnchorPaddingValueToken(decodePrf(
                      "A3308597F3C5271D5BAB640F749E619E9272A2C33F4CD372680F55F84CC4DF7F"_sd)),
                  anchorPaddingTokenValue);

    auto edcTextExactToken = EDCTextExactToken::deriveFrom(edcToken);
    ASSERT_EQUALS(EDCTextExactToken(decodePrf(
                      "17dde6bd0c0d783aa2bf84e255b162b9362032e5ebd6d655d6b478c4d77dc077"_sd)),
                  edcTextExactToken);
    auto edcTextSubstringToken = EDCTextSubstringToken::deriveFrom(edcToken);
    ASSERT_EQUALS(EDCTextSubstringToken(decodePrf(
                      "4dde679aa0568701a0fda6b1cae21e99da32500541e4ad832ea83db94497478f"_sd)),
                  edcTextSubstringToken);
    auto edcTextSuffixToken = EDCTextSuffixToken::deriveFrom(edcToken);
    ASSERT_EQUALS(EDCTextSuffixToken(decodePrf(
                      "61fa8b8f02a5e7f3cfd2c3e58d3fb8c2d1bfe8a1acc32e43f26478a52944af78"_sd)),
                  edcTextSuffixToken);
    auto edcTextPrefixToken = EDCTextPrefixToken::deriveFrom(edcToken);
    ASSERT_EQUALS(EDCTextPrefixToken(decodePrf(
                      "926e96d7142b2d187d10579a26a11499d6c30aac2e3fdd56eb1cd536875decfd"_sd)),
                  edcTextPrefixToken);

    auto escTextExactToken = ESCTextExactToken::deriveFrom(escToken);
    ASSERT_EQUALS(ESCTextExactToken(decodePrf(
                      "2fea10a92e84cce913ea0ffd7fd59964507e9e96cdffa4f1b861521f3e653260"_sd)),
                  escTextExactToken);
    auto escTextSubstringToken = ESCTextSubstringToken::deriveFrom(escToken);
    ASSERT_EQUALS(ESCTextSubstringToken(decodePrf(
                      "d6a94cacc9f5dd10b2b980bd4c4044e16ff1b29ad50c692e603487c46cbe610e"_sd)),
                  escTextSubstringToken);
    auto escTextSuffixToken = ESCTextSuffixToken::deriveFrom(escToken);
    ASSERT_EQUALS(ESCTextSuffixToken(decodePrf(
                      "336f39da6fa984a3477261ea19147b77f02e843f82511c94a91ec77bd72dca68"_sd)),
                  escTextSuffixToken);
    auto escTextPrefixToken = ESCTextPrefixToken::deriveFrom(escToken);
    ASSERT_EQUALS(ESCTextPrefixToken(decodePrf(
                      "2575c2a275e8135ec73093ae2edc793f6a3a0b8ed89767c3a8695ad93f1c6e9e"_sd)),
                  escTextPrefixToken);

    auto serverTextExactToken = ServerTextExactToken::deriveFrom(serverTokenDerivationToken);
    ASSERT_EQUALS(ServerTextExactToken(decodePrf(
                      "2ae33773be27c9fe3522ff0459f621670e93a7423166e63e9a43687a503c7438"_sd)),
                  serverTextExactToken);
    auto serverTextSubstringToken =
        ServerTextSubstringToken::deriveFrom(serverTokenDerivationToken);
    ASSERT_EQUALS(ServerTextSubstringToken(decodePrf(
                      "183ff338509675d5377d09978e9becd31b197458e2c8cd45b670c672ceb6dc53"_sd)),
                  serverTextSubstringToken);
    auto serverTextSuffixToken = ServerTextSuffixToken::deriveFrom(serverTokenDerivationToken);
    ASSERT_EQUALS(ServerTextSuffixToken(decodePrf(
                      "baf7c9392bb37607c6aa1f04163f4db8628872e7e7122e754bcd72771934ccbd"_sd)),
                  serverTextSuffixToken);
    auto serverTextPrefixToken = ServerTextPrefixToken::deriveFrom(serverTokenDerivationToken);
    ASSERT_EQUALS(ServerTextPrefixToken(decodePrf(
                      "52387ea6942d9299a89f70a2a4d8a4209ea1dfe56e29f6c0e3182ca7818b5c9c"_sd)),
                  serverTextPrefixToken);

    auto edcTextExactDerivedFromDataToken =
        EDCTextExactDerivedFromDataToken::deriveFrom(edcTextExactToken, sampleValue);
    ASSERT_EQUALS(EDCTextExactDerivedFromDataToken(decodePrf(
                      "6006D61E9E985EE8F6490AEBF0BFD120BF7A94317646165584894DA6ECBF0E97"_sd)),
                  edcTextExactDerivedFromDataToken);
    auto edcTextSubstringDerivedFromDataToken =
        EDCTextSubstringDerivedFromDataToken::deriveFrom(edcTextSubstringToken, sampleValue);
    ASSERT_EQUALS(EDCTextSubstringDerivedFromDataToken(decodePrf(
                      "4DAB3B274AA5D5E99ECBD7F139CD420F25305C9F082B25AA96F1C9FE6E2122E3"_sd)),
                  edcTextSubstringDerivedFromDataToken);
    auto edcTextSuffixDerivedFromDataToken =
        EDCTextSuffixDerivedFromDataToken::deriveFrom(edcTextSuffixToken, sampleValue);
    ASSERT_EQUALS(EDCTextSuffixDerivedFromDataToken(decodePrf(
                      "997F4A0B518F6C872951B65DCF26676D3886C98502FE0A7CC4F9C5EA4F30E94B"_sd)),
                  edcTextSuffixDerivedFromDataToken);
    auto edcTextPrefixDerivedFromDataToken =
        EDCTextPrefixDerivedFromDataToken::deriveFrom(edcTextPrefixToken, sampleValue);
    ASSERT_EQUALS(EDCTextPrefixDerivedFromDataToken(decodePrf(
                      "D3EBDD34BCACD11C5929A09D2A897EB7938ABD4756CAB805F46FF0F4AFFA7583"_sd)),
                  edcTextPrefixDerivedFromDataToken);

    auto edcTextExactDerivedFromDataTokenAndContentionFactorToken =
        EDCTextExactDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
            edcTextExactDerivedFromDataToken, counter);
    ASSERT_EQUALS(EDCTextExactDerivedFromDataTokenAndContentionFactorToken(decodePrf(
                      "3551507189d32cc7768390cdd83071deeb3055ca86c4756b16bb740024b23610"_sd)),
                  edcTextExactDerivedFromDataTokenAndContentionFactorToken);
    auto edcTextSubstringDerivedFromDataTokenAndContentionFactorToken =
        EDCTextSubstringDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
            edcTextSubstringDerivedFromDataToken, counter);
    ASSERT_EQUALS(EDCTextSubstringDerivedFromDataTokenAndContentionFactorToken(decodePrf(
                      "4b52f6daf3b688971eb5819820c3468b3c79ba45fd3e86134351f9baf203e0d1"_sd)),
                  edcTextSubstringDerivedFromDataTokenAndContentionFactorToken);
    auto edcTextSuffixDerivedFromDataTokenAndContentionFactorToken =
        EDCTextSuffixDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
            edcTextSuffixDerivedFromDataToken, counter);
    ASSERT_EQUALS(EDCTextSuffixDerivedFromDataTokenAndContentionFactorToken(decodePrf(
                      "c1f273913631a9a1b36fea884b3c8a3a141c9e21556981094ba15f262e540ac7"_sd)),
                  edcTextSuffixDerivedFromDataTokenAndContentionFactorToken);
    auto edcTextPrefixDerivedFromDataTokenAndContentionFactorToken =
        EDCTextPrefixDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
            edcTextPrefixDerivedFromDataToken, counter);
    ASSERT_EQUALS(EDCTextPrefixDerivedFromDataTokenAndContentionFactorToken(decodePrf(
                      "a8d68ebdb0e2d31754f693c56071b0b1d225e6ec5568aff875bd4f7c48c24f66"_sd)),
                  edcTextPrefixDerivedFromDataTokenAndContentionFactorToken);

    auto escTextExactDerivedFromDataToken =
        ESCTextExactDerivedFromDataToken::deriveFrom(escTextExactToken, sampleValue);
    ASSERT_EQUALS(ESCTextExactDerivedFromDataToken(decodePrf(
                      "6D3311B3DE0E32DBFE55A565327AD4B99D670474DDEF52AA200FC79D76B8C7C4"_sd)),
                  escTextExactDerivedFromDataToken);
    auto escTextSubstringDerivedFromDataToken =
        ESCTextSubstringDerivedFromDataToken::deriveFrom(escTextSubstringToken, sampleValue);
    ASSERT_EQUALS(ESCTextSubstringDerivedFromDataToken(decodePrf(
                      "0E9D0F7C42658DD6894D3CBE34FFA0D39BE00BA72A21AC79BC3712B25783D247"_sd)),
                  escTextSubstringDerivedFromDataToken);
    auto escTextSuffixDerivedFromDataToken =
        ESCTextSuffixDerivedFromDataToken::deriveFrom(escTextSuffixToken, sampleValue);
    ASSERT_EQUALS(ESCTextSuffixDerivedFromDataToken(decodePrf(
                      "A95860B4A08B39B002E5AEE557268786BE4E6D5A8552090DC397FBDB34374313"_sd)),
                  escTextSuffixDerivedFromDataToken);
    auto escTextPrefixDerivedFromDataToken =
        ESCTextPrefixDerivedFromDataToken::deriveFrom(escTextPrefixToken, sampleValue);
    ASSERT_EQUALS(ESCTextPrefixDerivedFromDataToken(decodePrf(
                      "8DF7341DD42C1CEE8411657AFAEA424BFA818A539BF0668E1C355FC2A555E11A"_sd)),
                  escTextPrefixDerivedFromDataToken);

    auto escTextExactDerivedFromDataTokenAndContentionFactorToken =
        ESCTextExactDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
            escTextExactDerivedFromDataToken, counter);
    ASSERT_EQUALS(ESCTextExactDerivedFromDataTokenAndContentionFactorToken(decodePrf(
                      "4cd82a4883ab0a6224a24937066f94827f5107e8bb2f0fa841e10aff8d49e8e4"_sd)),
                  escTextExactDerivedFromDataTokenAndContentionFactorToken);
    auto escTextSubstringDerivedFromDataTokenAndContentionFactorToken =
        ESCTextSubstringDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
            escTextSubstringDerivedFromDataToken, counter);
    ASSERT_EQUALS(ESCTextSubstringDerivedFromDataTokenAndContentionFactorToken(decodePrf(
                      "94f116183f14442e335902756e5b730683cd998c683b90d04dc9e8f48684bdb2"_sd)),
                  escTextSubstringDerivedFromDataTokenAndContentionFactorToken);
    auto escTextSuffixDerivedFromDataTokenAndContentionFactorToken =
        ESCTextSuffixDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
            escTextSuffixDerivedFromDataToken, counter);
    ASSERT_EQUALS(ESCTextSuffixDerivedFromDataTokenAndContentionFactorToken(decodePrf(
                      "13390024c674c2d131771cf95af9787c8c7ef76b5d63078b3dc482cb4075a634"_sd)),
                  escTextSuffixDerivedFromDataTokenAndContentionFactorToken);
    auto escTextPrefixDerivedFromDataTokenAndContentionFactorToken =
        ESCTextPrefixDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
            escTextPrefixDerivedFromDataToken, counter);
    ASSERT_EQUALS(ESCTextPrefixDerivedFromDataTokenAndContentionFactorToken(decodePrf(
                      "9d0d5f84de779360c43be8e61710deb4b2705b6bf1e11ee41fba09fd4486b7c6"_sd)),
                  escTextPrefixDerivedFromDataTokenAndContentionFactorToken);

    auto serverTextExactDerivedFromDataToken =
        ServerTextExactDerivedFromDataToken::deriveFrom(serverTextExactToken, sampleValue);
    ASSERT_EQUALS(ServerTextExactDerivedFromDataToken(decodePrf(
                      "ae210cd90c230bb8cb96a4e840c9cf88740ae156f3514260d3f1ce94b0bf941e"_sd)),
                  serverTextExactDerivedFromDataToken);
    auto serverTextSubstringDerivedFromDataToken =
        ServerTextSubstringDerivedFromDataToken::deriveFrom(serverTextSubstringToken, sampleValue);
    ASSERT_EQUALS(ServerTextSubstringDerivedFromDataToken(decodePrf(
                      "7b514ede2cd6d364f2da2580bf173a4e68f9e0617c123aee8b9dda4d8d4b47d7"_sd)),
                  serverTextSubstringDerivedFromDataToken);
    auto serverTextSuffixDerivedFromDataToken =
        ServerTextSuffixDerivedFromDataToken::deriveFrom(serverTextSuffixToken, sampleValue);
    ASSERT_EQUALS(ServerTextSuffixDerivedFromDataToken(decodePrf(
                      "3e242be8d4c8a5894e81aa5fe0729cf48355dbe219c5c6b5ceb8b0eef124ba40"_sd)),
                  serverTextSuffixDerivedFromDataToken);
    auto serverTextPrefixDerivedFromDataToken =
        ServerTextPrefixDerivedFromDataToken::deriveFrom(serverTextPrefixToken, sampleValue);
    ASSERT_EQUALS(ServerTextPrefixDerivedFromDataToken(decodePrf(
                      "8d8d41ac0618b0e98b086d662a2466f4aa1527d6536acdbcf220c724073331eb"_sd)),
                  serverTextPrefixDerivedFromDataToken);
}

TEST_F(ServiceContextTest, FLETokens_TestVectorESCCollectionDecryptDocument) {
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

TEST_F(ServiceContextTest, FLE_ESC_RoundTrip) {
    TestKeyVault keyVault;

    ConstDataRange value(testValue);

    auto c1 = CollectionsLevel1Token::deriveFrom(getIndexKey());
    auto escToken = ESCToken::deriveFrom(c1);

    ESCDerivedFromDataToken escDatakey = ESCDerivedFromDataToken::deriveFrom(escToken, value);

    ESCDerivedFromDataTokenAndContentionFactorToken escDataCounterkey =
        ESCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(escDatakey, 0);

    auto escTwiceTag = ESCTwiceDerivedTagToken::deriveFrom(escDataCounterkey);
    auto escTwiceValue = ESCTwiceDerivedValueToken::deriveFrom(escDataCounterkey);


    {
        BSONObj doc = ESCCollection::generateNullDocument(
            &hmacCtx, escTwiceTag, escTwiceValue, 123, 123456789);
        auto swDoc = ESCCollection::decryptNullDocument(escTwiceValue, doc);
        ASSERT_OK(swDoc.getStatus());
        ASSERT_EQ(swDoc.getValue().position, 123);
        ASSERT_EQ(swDoc.getValue().count, 123456789);
    }


    {
        BSONObj doc = ESCCollection::generateInsertDocument(
            &hmacCtx, escTwiceTag, escTwiceValue, 123, 123456789);
        auto swDoc = ESCCollection::decryptDocument(escTwiceValue, doc);
        ASSERT_OK(swDoc.getStatus());
        ASSERT_EQ(swDoc.getValue().compactionPlaceholder, false);
        ASSERT_EQ(swDoc.getValue().position, 0);
        ASSERT_EQ(swDoc.getValue().count, 123456789);
    }

    {
        BSONObj doc = ESCCollection::generateCompactionPlaceholderDocument(
            &hmacCtx, escTwiceTag, escTwiceValue, 123, 456789);
        auto swDoc = ESCCollection::decryptDocument(escTwiceValue, doc);
        ASSERT_OK(swDoc.getStatus());
        ASSERT_EQ(swDoc.getValue().compactionPlaceholder, true);
        ASSERT_EQ(swDoc.getValue().position, std::numeric_limits<uint64_t>::max());
        ASSERT_EQ(swDoc.getValue().count, 456789);
    }

    {
        // Non-anchor documents don't work with decryptAnchorDocument()
        BSONObj doc = ESCCollection::generateNonAnchorDocument(&hmacCtx, escTwiceTag, 123);
        auto swDoc = ESCCollection::decryptAnchorDocument(escTwiceValue, doc);
        ASSERT_NOT_OK(swDoc.getStatus());
        ASSERT_EQ(ErrorCodes::Error::NoSuchKey, swDoc.getStatus().code());
    }

    {
        BSONObj doc = ESCCollection::generateAnchorDocument(
            &hmacCtx, escTwiceTag, escTwiceValue, 123, 456789);
        auto swDoc = ESCCollection::decryptAnchorDocument(escTwiceValue, doc);
        ASSERT_OK(swDoc.getStatus());
        ASSERT_EQ(swDoc.getValue().position, 0);
        ASSERT_EQ(swDoc.getValue().count, 456789);
    }

    {
        BSONObj doc = ESCCollection::generateNullAnchorDocument(
            &hmacCtx, escTwiceTag, escTwiceValue, 123, 456789);
        auto swDoc = ESCCollection::decryptAnchorDocument(escTwiceValue, doc);
        ASSERT_OK(swDoc.getStatus());
        ASSERT_EQ(swDoc.getValue().position, 123);
        ASSERT_EQ(swDoc.getValue().count, 456789);
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

    ECStats getStats() const override {
        return ECStats();
    }

    void setOverrideCount(int64_t count) {
        _overrideCount = count;
    }

private:
    std::vector<BSONObj> _docs;
    boost::optional<int64_t> _overrideCount;
};

namespace {

std::tuple<ESCTwiceDerivedTagToken, ESCTwiceDerivedValueToken> generateEmuBinaryTokens(
    ConstDataRange value, uint64_t contention = 0) {
    auto c1 = CollectionsLevel1Token::deriveFrom(getIndexKey());
    auto escToken = ESCToken::deriveFrom(c1);

    ESCDerivedFromDataToken escDatakey = ESCDerivedFromDataToken::deriveFrom(escToken, value);

    auto escDerivedToken =
        ESCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(escDatakey, contention);

    auto escTwiceTag = ESCTwiceDerivedTagToken::deriveFrom(escDerivedToken);
    auto escTwiceValue = ESCTwiceDerivedValueToken::deriveFrom(escDerivedToken);
    return std::tie(escTwiceTag, escTwiceValue);
}

EmuBinaryResult EmuBinaryV2Test(boost::optional<std::pair<uint64_t, uint64_t>> nullAnchor,
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
        auto doc = ESCCollection::generateNullAnchorDocument(
            &hmacCtx, tagToken, valueToken, nullApos, nullCpos);
        coll.insert(doc);
    }

    ASSERT_LESS_THAN_OR_EQUALS(anchorCposStart, anchorCposEnd);

    // insert regular anchors with positions between anchorStart and anchorEnd (exclusive)
    uint64_t lastAnchorCpos = anchorCposStart;
    auto anchorEnd = anchorStart + anchorCount;
    for (auto apos = anchorStart; apos < anchorEnd; apos++) {
        auto doc = ESCCollection::generateAnchorDocument(
            &hmacCtx, tagToken, valueToken, apos, lastAnchorCpos);
        coll.insert(doc);
        if (lastAnchorCpos < anchorCposEnd) {
            lastAnchorCpos++;
        }
    }

    // insert non-anchors with positions between nonAnchorStart and nonAnchorEnd (exclusive)
    uint64_t nonAnchorEnd = nonAnchorStart + nonAnchorCount;
    for (auto cpos = nonAnchorStart; cpos < nonAnchorEnd; cpos++) {
        auto doc = ESCCollection::generateNonAnchorDocument(&hmacCtx, tagToken, cpos);
        coll.insert(doc);
    }

    HmacContext hmacCtx;
    auto res = ESCCollection::emuBinaryV2(&hmacCtx, coll, tagToken, valueToken);

    return res;
}
}  // namespace

// Test EmuBinaryV2 on empty collection
TEST_F(ServiceContextTest, FLE_ESC_EmuBinaryV2_Empty) {
    auto res = EmuBinaryV2Test(boost::none, 0, 0, 0, 0, 0, 0);
    ASSERT_TRUE(res.apos.has_value());
    ASSERT_EQ(res.apos.value(), 0);
    ASSERT_TRUE(res.cpos.has_value());
    ASSERT_EQ(res.cpos.value(), 0);
}

// Test EmuBinaryV2 on ESC containing non-anchors only
TEST_F(ServiceContextTest, FLE_ESC_EmuBinaryV2_NonAnchorsOnly) {
    auto res = EmuBinaryV2Test(boost::none, 0, 0, 0, 0, 1, 5);
    ASSERT_TRUE(res.apos.has_value());
    ASSERT_EQ(res.apos.value(), 0);
    ASSERT_TRUE(res.cpos.has_value());
    ASSERT_EQ(res.cpos.value(), 5);
}

// Test EmuBinaryV2 on ESC containing non-null anchors only
TEST_F(ServiceContextTest, FLE_ESC_EmuBinaryV2_RegularAnchorsOnly) {
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
TEST_F(ServiceContextTest, FLE_ESC_EmuBinaryV2_NonAnchorsAndRegularAnchorsOnly) {

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
TEST_F(ServiceContextTest, FLE_ESC_EmuBinaryV2_NullAnchorOnly) {
    std::vector<std::pair<uint64_t, uint64_t>> nullAnchors = {
        {0, 0}, {0, 10}, {10, 0}, {10, 10}, {5, 10}, {10, 5}};

    for (auto& anchor : nullAnchors) {
        auto res = EmuBinaryV2Test(anchor, 0, 0, 0, 0, 0, 0);
        ASSERT_FALSE(res.apos.has_value());
        ASSERT_FALSE(res.cpos.has_value());
    }
}

// Test EmuBinaryV2 on ESC containing null anchor and non-anchors only
TEST_F(ServiceContextTest, FLE_ESC_EmuBinaryV2_NullAnchorAndNonAnchorsOnly) {

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
TEST_F(ServiceContextTest, FLE_ESC_EmuBinaryV2_NullAndRegularAnchorsOnly) {

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
TEST_F(ServiceContextTest, FLE_ESC_EmuBinaryV2_AllRecordTypes_NullAnchorHasNewerPositions) {
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
TEST_F(ServiceContextTest, FLE_ESC_EmuBinaryV2_AllRecordTypes_NullAnchorHasLastAnchorPositions) {
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
TEST_F(ServiceContextTest, FLE_ESC_EmuBinaryV2_AllRecordTypes_NullAnchorHasOldAnchorPositions) {
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

enum class Operation { kFind, kInsert };

EncryptedFieldConfig getTestEncryptedFieldConfig() {

    constexpr auto schema = R"({
        "escCollection": "enxcol_.coll.esc",
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

    return EncryptedFieldConfig::parse(fromjson(schema), IDLParserContext("root"));
}

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
        case BSONType::numberInt:
            lowerDoc = BSON("lb" << 0);
            upperDoc = BSON("ub" << 1234567);
            break;
        case BSONType::numberLong:
            lowerDoc = BSON("lb" << 0LL);
            upperDoc = BSON("ub" << 1234567890123456789LL);
            break;
        case BSONType::numberDouble:
            lowerDoc = BSON("lb" << 0.0);
            upperDoc = BSON("ub" << 1234567890123456789.0);
            break;
        case BSONType::date:
            lowerDoc = BSON("lb" << Date_t::fromMillisSinceEpoch(0));
            upperDoc = BSON("ub" << Date_t::fromMillisSinceEpoch(1234567890123456789LL));
            break;
        case BSONType::numberDecimal:
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
    insertSpec.setMinBound(boost::optional<IDLAnyType>(lowerDoc.firstElement()));
    insertSpec.setMaxBound(boost::optional<IDLAnyType>(upperDoc.firstElement()));
    auto specDoc = BSON("s" << insertSpec.toBSON());

    FLE2RangeFindSpecEdgesInfo edgesInfo;
    FLE2RangeFindSpec findSpec;

    edgesInfo.setLowerBound(lowerDoc.firstElement());
    edgesInfo.setLbIncluded(true);
    edgesInfo.setUpperBound(upperDoc.firstElement());
    edgesInfo.setUbIncluded(true);
    edgesInfo.setIndexMin(lowerDoc.firstElement());
    edgesInfo.setIndexMax(upperDoc.firstElement());
    edgesInfo.setTrimFactor(1);

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
    ASSERT_EQ(finalDoc[kSafeContent].type(), BSONType::array);
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

BSONObj transformElementForInsertUpdate(BSONElement element,
                                        const std::vector<char>& placeholder,
                                        const EncryptedFieldConfig& efc,
                                        const NamespaceString& edcNs,
                                        FLEKeyVault* kv) {
    // Wrap the element in a document in an insert command, so libmongocrypt can transform
    // the placeholders.
    auto origCmd = write_ops::InsertCommandRequest(edcNs, {element.wrap()}).toBSON();
    auto cryptdResponse = [&]() {
        BSONObjBuilder docbob;
        docbob.appendBinData(element.fieldNameStringData(),
                             placeholder.size(),
                             BinDataType::Encrypt,
                             placeholder.data());
        BSONObjBuilder bob;
        bob.append("hasEncryptionPlaceholders", true);
        bob.append("schemaRequiresEncryption", true);
        bob.append("result", write_ops::InsertCommandRequest(edcNs, {docbob.obj()}).toBSON());
        return bob.obj();
    }();
    auto finalCmd =
        FLEClientCrypto::transformPlaceholders(origCmd,
                                               cryptdResponse,
                                               BSON(edcNs.toString_forTest() << efc.toBSON()),
                                               kv,
                                               edcNs.db_forTest())
            .addField(BSON("$db" << edcNs.db_forTest()).firstElement());
    return write_ops::InsertCommandRequest::parse(finalCmd, IDLParserContext("finalCmd"))
        .getDocuments()
        .front()
        .getOwned();
}

void roundTripTest(BSONObj doc, BSONType type, Operation opType, Fle2AlgorithmInt algorithm) {
    auto element = doc.firstElement();
    ASSERT_EQ(element.type(), type);

    TestKeyVault keyVault;

    auto efc = getTestEncryptedFieldConfig();
    auto edcNs = NamespaceString::createNamespaceString_forTest("test.coll");

    auto inputDoc = BSON("plainText" << "sample"
                                     << "encrypted" << element);

    auto buf = generatePlaceholder(element, opType, algorithm);
    BSONObjBuilder builder;
    builder.append("plainText", "sample");
    builder.appendAs(
        transformElementForInsertUpdate(element, buf, efc, edcNs, &keyVault).firstElement(),
        "encrypted");
    auto transformedDoc = builder.obj();

    auto finalDoc = [&]() {
        auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(transformedDoc);

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
        auto finalDoc = EDCServerCollection::finalizeForInsert(transformedDoc, serverPayload);
        ASSERT_EQ(finalDoc[kSafeContent].type(), BSONType::array);
        return finalDoc;
    }();

    ASSERT_EQ(finalDoc["plainText"].type(), BSONType::string);
    ASSERT_EQ(finalDoc["encrypted"].type(), BSONType::binData);
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

    auto inputDoc = BSON("plainText" << "sample"
                                     << "encrypted1" << element1 << "encrypted2" << element2);

    auto buf1 = generatePlaceholder(element1, operation1, Fle2AlgorithmInt::kEquality, indexKeyId);
    auto buf2 = generatePlaceholder(element2, operation2, Fle2AlgorithmInt::kEquality, indexKey2Id);

    BSONObjBuilder builder;
    builder.append("plaintext", "sample");
    builder.appendBinData("encrypted1", buf1.size(), BinDataType::Encrypt, buf1.data());
    builder.appendBinData("encrypted2", buf2.size(), BinDataType::Encrypt, buf2.data());

    auto finalDoc = encryptDocument(builder.obj(), &keyVault);

    ASSERT_EQ(finalDoc["encrypted1"].type(), BSONType::binData);
    ASSERT_TRUE(finalDoc["encrypted1"].isBinData(BinDataType::Encrypt));

    ASSERT_EQ(finalDoc["encrypted2"].type(), BSONType::binData);
    ASSERT_TRUE(finalDoc["encrypted2"].isBinData(BinDataType::Encrypt));

    assertPayload(finalDoc["encrypted1"], operation1);
    assertPayload(finalDoc["encrypted2"], operation2);
}

// Used to generate the test data for the ExpressionFLETest in expression_test.cpp
TEST_F(ServiceContextTest, FLE_EDC_PrintTest) {
    auto doc = BSON("value" << 1);
    auto element = doc.firstElement();

    TestKeyVault keyVault;

    auto inputDoc = BSON("plainText" << "sample"
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

TEST_F(ServiceContextTest, FLE_EDC_Allowed_Types) {
    const std::vector<std::pair<BSONObj, BSONType>> universallyAllowedObjects{
        {BSON("sample" << "value123"), BSONType::string},
        {BSON("sample" << BSONBinData(
                  testValue.data(), testValue.size(), BinDataType::BinDataGeneral)),
         BSONType::binData},
        {BSON("sample" << OID()), BSONType::oid},
        {BSON("sample" << false), BSONType::boolean},
        {BSON("sample" << true), BSONType::boolean},
        {BSON("sample" << Date_t()), BSONType::date},
        {BSON("sample" << BSONRegEx("value1", "lu")), BSONType::regEx},
        {BSON("sample" << 123456), BSONType::numberInt},
        {BSON("sample" << Timestamp()), BSONType::timestamp},
        {BSON("sample" << 12345678901234567LL), BSONType::numberLong},
        {BSON("sample" << BSONCode("value")), BSONType::code}};

    const std::vector<std::pair<BSONObj, BSONType>> unindexedAllowedObjects{
        {BSON("sample" << 123.456), BSONType::numberDouble},
        {BSON("sample" << Decimal128()), BSONType::numberDecimal},
        {BSON("sample" << BSON("nested" << "value")), BSONType::object},
        {BSON("sample" << BSON_ARRAY(1 << 23)), BSONType::array},
        {BSON("sample" << BSONDBRef("value1", OID())), BSONType::dbRef},
        {BSON("sample" << BSONSymbol("value")), BSONType::symbol},
        {BSON("sample" << BSONCodeWScope("value", BSON("code" << "something"))),
         BSONType::codeWScope},
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

TEST_F(ServiceContextTest, FLE_EDC_Range_Allowed_Types) {

    const std::vector<std::pair<BSONObj, BSONType>> rangeAllowedObjects{
        {BSON("sample" << 123.456), BSONType::numberDouble},
        {BSON("sample" << Decimal128()), BSONType::numberDecimal},
        {BSON("sample" << 123456), BSONType::numberInt},
        {BSON("sample" << 12345678901234567LL), BSONType::numberLong},
        {BSON("sample" << Date_t::fromMillisSinceEpoch(12345)), BSONType::date},
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

TEST_F(ServiceContextTest, FLE_EDC_Disallowed_Types) {
    illegalBSONType(BSON("sample" << 123.456), BSONType::numberDouble, Fle2AlgorithmInt::kEquality);
    illegalBSONType(
        BSON("sample" << Decimal128()), BSONType::numberDecimal, Fle2AlgorithmInt::kEquality);

    illegalBSONType(BSON("sample" << MINKEY), BSONType::minKey, Fle2AlgorithmInt::kEquality);

    illegalBSONType(
        BSON("sample" << BSON("nested" << "value")), BSONType::object, Fle2AlgorithmInt::kEquality);
    illegalBSONType(
        BSON("sample" << BSON_ARRAY(1 << 23)), BSONType::array, Fle2AlgorithmInt::kEquality);

    illegalBSONType(
        BSON("sample" << BSONUndefined), BSONType::undefined, Fle2AlgorithmInt::kEquality);
    illegalBSONType(
        BSON("sample" << BSONUndefined), BSONType::undefined, Fle2AlgorithmInt::kUnindexed);
    illegalBSONType(BSON("sample" << BSONNULL), BSONType::null, Fle2AlgorithmInt::kEquality);
    illegalBSONType(BSON("sample" << BSONNULL), BSONType::null, Fle2AlgorithmInt::kUnindexed);
    illegalBSONType(BSON("sample" << BSONCodeWScope("value", BSON("code" << "something"))),
                    BSONType::codeWScope,
                    Fle2AlgorithmInt::kEquality);
    illegalBSONType(BSON("sample" << MAXKEY), BSONType::maxKey, Fle2AlgorithmInt::kEquality);
    illegalBSONType(BSON("sample" << MAXKEY), BSONType::maxKey, Fle2AlgorithmInt::kUnindexed);
}

void illegalRangeBSONType(BSONObj doc, BSONType type) {
    illegalBSONType(doc, type, Fle2AlgorithmInt::kRange, ErrorCodes::TypeMismatch);
}

TEST_F(ServiceContextTest, FLE_EDC_Range_Disallowed_Types) {

    const std::vector<std::pair<BSONObj, BSONType>> disallowedObjects{
        {BSON("sample" << "value123"), BSONType::string},
        {BSON("sample" << BSONBinData(
                  testValue.data(), testValue.size(), BinDataType::BinDataGeneral)),
         BSONType::binData},
        {BSON("sample" << OID()), BSONType::oid},
        {BSON("sample" << false), BSONType::boolean},
        {BSON("sample" << true), BSONType::boolean},
        {BSON("sample" << BSONRegEx("value1", "value2")), BSONType::regEx},
        {BSON("sample" << Timestamp()), BSONType::timestamp},
        {BSON("sample" << BSONCode("value")), BSONType::code},
        {BSON("sample" << BSON("nested" << "value")), BSONType::object},
        {BSON("sample" << BSON_ARRAY(1 << 23)), BSONType::array},
        {BSON("sample" << BSONDBRef("value1", OID())), BSONType::dbRef},
        {BSON("sample" << BSONSymbol("value")), BSONType::symbol},
        {BSON("sample" << BSONCodeWScope("value", BSON("code" << "something"))),
         BSONType::codeWScope},
        {BSON("sample" << MINKEY), BSONType::minKey},
        {BSON("sample" << MAXKEY), BSONType::maxKey},
    };

    for (const auto& typePair : disallowedObjects) {
        illegalRangeBSONType(typePair.first, typePair.second);
    }

    illegalBSONType(BSON("sample" << BSONNULL),
                    BSONType::null,
                    Fle2AlgorithmInt::kRange,
                    ErrorCodes::IDLFailedToParse);
    illegalBSONType(BSON("sample" << BSONUndefined),
                    BSONType::undefined,
                    Fle2AlgorithmInt::kRange,
                    ErrorCodes::IDLFailedToParse);
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
            if (elem.type() == BSONType::object) {
                frameStack.push({BSONObjIterator(elem.Obj()),
                                 BSONObjBuilder(builder.subobjStart(elem.fieldNameStringData()))});
            } else if (elem.type() == BSONType::array) {
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


    auto inputDoc = BSON("plainText" << "sample"
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

            iup.setType(stdx::to_underlying(type));
            toEncryptedBinData(fieldNameToSerialize,
                               EncryptedBinDataType::kFLE2InsertUpdatePayloadV2,
                               iup,
                               builder);
        });


    // Start Server Side
    ASSERT_THROWS_CODE(EDCServerCollection::getEncryptedFieldInfo(result), DBException, 6373504);
}

TEST_F(ServiceContextTest, FLE_EDC_Disallowed_Types_FLE2InsertUpdatePayload) {
    disallowedEqualityPayloadType(BSONType::numberDouble);
    disallowedEqualityPayloadType(BSONType::numberDecimal);

    disallowedEqualityPayloadType(BSONType::minKey);

    disallowedEqualityPayloadType(BSONType::object);
    disallowedEqualityPayloadType(BSONType::array);

    disallowedEqualityPayloadType(BSONType::undefined);
    disallowedEqualityPayloadType(BSONType::null);
    disallowedEqualityPayloadType(BSONType::codeWScope);

    disallowedEqualityPayloadType(BSONType::maxKey);

    uint8_t fakeBSONType = 42;
    ASSERT_FALSE(isValidBSONType(fakeBSONType));
    disallowedEqualityPayloadType(static_cast<BSONType>(fakeBSONType));
}

bool areBuffersEqual(ConstDataRange lhs, ConstDataRange rhs) {
    return (lhs.length() == rhs.length()) &&
        std::equal(lhs.data<const uint8_t>(),
                   lhs.data<const uint8_t>() + lhs.length(),
                   rhs.data<const uint8_t>());
}

bool operator==(const ConstFLE2TagAndEncryptedMetadataBlock& lblk,
                const ConstFLE2TagAndEncryptedMetadataBlock& rblk) {
    auto lhs = lblk.getView();
    auto rhs = rblk.getView();
    return areBuffersEqual(lhs.encryptedCounts, rhs.encryptedCounts) &&
        areBuffersEqual(lhs.encryptedZeros, rhs.encryptedZeros) &&
        areBuffersEqual(lhs.tag, rhs.tag);
}

bool metadataBlocksEqual(const std::vector<ConstFLE2TagAndEncryptedMetadataBlock>& lhs,
                         const std::vector<ConstFLE2TagAndEncryptedMetadataBlock>& rhs) {
    return (lhs.size() == rhs.size()) && std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

bool operator==(const FLE2IndexedEqualityEncryptedValueV2& lhs,
                const FLE2IndexedEqualityEncryptedValueV2& rhs) {
    return (lhs.getBsonType() == rhs.getBsonType()) && (lhs.getKeyId() == rhs.getKeyId()) &&
        (lhs.getRawMetadataBlock() == rhs.getRawMetadataBlock()) &&
        areBuffersEqual(lhs.getServerEncryptedValue(), rhs.getServerEncryptedValue()) &&
        areBuffersEqual(lhs.getMetadataBlockTag(), rhs.getMetadataBlockTag());
}

bool operator==(const FLE2IndexedRangeEncryptedValueV2& lhs,
                const FLE2IndexedRangeEncryptedValueV2& rhs) {
    return (lhs.getBsonType() == rhs.getBsonType()) && (lhs.getKeyId() == rhs.getKeyId()) &&
        (lhs.getTagCount() == rhs.getTagCount()) &&
        areBuffersEqual(lhs.getServerEncryptedValue(), rhs.getServerEncryptedValue()) &&
        metadataBlocksEqual(lhs.getMetadataBlocks(), rhs.getMetadataBlocks());
}

bool operator==(const FLE2IndexedTextEncryptedValue& lhs,
                const FLE2IndexedTextEncryptedValue& rhs) {
    return (lhs.getBsonType() == rhs.getBsonType()) && (lhs.getKeyId() == rhs.getKeyId()) &&
        (lhs.getTagCount() == rhs.getTagCount()) &&
        (lhs.getSubstringTagCount() == rhs.getSubstringTagCount()) &&
        (lhs.getSuffixTagCount() == rhs.getSuffixTagCount()) &&
        (lhs.getPrefixTagCount() == rhs.getPrefixTagCount()) &&
        areBuffersEqual(lhs.getServerEncryptedValue(), rhs.getServerEncryptedValue()) &&
        (lhs.getExactStringMetadataBlock() == rhs.getExactStringMetadataBlock()) &&
        metadataBlocksEqual(lhs.getAllMetadataBlocks(), rhs.getAllMetadataBlocks()) &&
        metadataBlocksEqual(lhs.getSubstringMetadataBlocks(), rhs.getSubstringMetadataBlocks()) &&
        metadataBlocksEqual(lhs.getSuffixMetadataBlocks(), rhs.getSuffixMetadataBlocks()) &&
        metadataBlocksEqual(lhs.getPrefixMetadataBlocks(), rhs.getPrefixMetadataBlocks());
}

TEST_F(ServiceContextTest, FLE_EDC_ServerSide_Equality_Payloads_V2) {
    TestKeyVault keyVault;

    auto doc = BSON("sample" << 123456);
    auto element = doc.firstElement();

    auto value = ConstDataRange(element.value(), element.value() + element.valuesize());

    auto collectionToken = CollectionsLevel1Token::deriveFrom(getIndexKey());
    auto serverEncryptToken = ServerDataEncryptionLevel1Token::deriveFrom(getIndexKey());
    auto serverDerivationToken = ServerTokenDerivationLevel1Token::deriveFrom(getIndexKey());

    auto edcToken = EDCToken::deriveFrom(collectionToken);
    auto escToken = ESCToken::deriveFrom(collectionToken);
    auto ecocToken = ECOCToken::deriveFrom(collectionToken);
    auto serverDerivedFromDataToken =
        ServerDerivedFromDataToken::deriveFrom(serverDerivationToken, value);
    FLECounter contention = 3;

    EDCDerivedFromDataToken edcDatakey = EDCDerivedFromDataToken::deriveFrom(edcToken, value);
    ESCDerivedFromDataToken escDatakey = ESCDerivedFromDataToken::deriveFrom(escToken, value);

    ESCDerivedFromDataTokenAndContentionFactorToken escDataCounterkey =
        ESCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(escDatakey, contention);
    EDCDerivedFromDataTokenAndContentionFactorToken edcDataCounterkey =
        EDCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(edcDatakey, contention);

    FLE2InsertUpdatePayloadV2 iupayload;
    iupayload.setEdcDerivedToken(edcDataCounterkey);
    iupayload.setEscDerivedToken(escDataCounterkey);
    iupayload.setServerEncryptionToken(serverEncryptToken);
    iupayload.setServerDerivedFromDataToken(serverDerivedFromDataToken);

    auto encryptedTokens =
        StateCollectionTokensV2(escDataCounterkey, boost::none, boost::none).encrypt(ecocToken);
    ASSERT_EQ(encryptedTokens.toCDR().length(), crypto::aesCTRIVSize + sizeof(PrfBlock));
    iupayload.setEncryptedTokens(std::move(encryptedTokens));
    iupayload.setIndexKeyId(indexKeyId);

    iupayload.setValue(value);
    iupayload.setType(stdx::to_underlying(element.type()));

    iupayload.setContentionFactor(contention);

    auto edcTwiceDerived = EDCTwiceDerivedToken::deriveFrom(edcDataCounterkey);

    uint64_t counter = 123456;  // counter from ESC
    auto tag = EDCServerCollection::generateTag(&hmacCtx, edcTwiceDerived, counter);

    // serialize to on-disk format
    auto serverPayload =
        FLE2IndexedEqualityEncryptedValueV2::fromUnencrypted(iupayload, tag, counter);
    auto buf = uassertStatusOK(serverPayload.serialize());

    // We expect the serialized buffer to contain FLE subtype (1 byte), UUID (16), BSON type (1),
    // CTR-encrypted client value (16 byte IV + ciphertext) + metadata block (96)
    const size_t expectedSize = 1 + UUID::kNumBytes + 1 +
        (crypto::aesCTRIVSize + iupayload.getValue().length()) +
        sizeof(FLE2TagAndEncryptedMetadataBlock::SerializedBlob);
    ASSERT_EQ(buf.size(), expectedSize);

    // parse it back
    FLE2IndexedEqualityEncryptedValueV2 parsed(buf);

    // validate the original & parsed objects have identical values
    ASSERT_EQ(serverPayload, parsed);

    // validate individual fields have expected values
    ASSERT_EQ(parsed.getKeyId(), indexKeyId);
    ASSERT_EQ(parsed.getBsonType(), element.type());
    ASSERT_EQ(parsed.getMetadataBlockTag(), tag);

    // verify the server-encrypted value decrypts back to client-encrypted value
    auto clientEncryptedValue = uassertStatusOK(
        FLEUtil::decryptData(serverEncryptToken.toCDR(), parsed.getServerEncryptedValue()));
    ASSERT_EQ(clientEncryptedValue.size(), value.length());
    ASSERT(std::equal(
        clientEncryptedValue.begin(), clientEncryptedValue.end(), value.data<uint8_t>()));

    // verify the metadata blocks have the correct values after decrypt
    auto mblock = parsed.getRawMetadataBlock();
    auto zeros = uassertStatusOK(mblock.decryptZerosBlob(
        ServerZerosEncryptionToken::deriveFrom(serverDerivedFromDataToken)));
    ASSERT_TRUE(FLE2TagAndEncryptedMetadataBlock::isValidZerosBlob(zeros));

    auto counterPair = uassertStatusOK(mblock.decryptCounterAndContentionFactorPair(
        ServerCountAndContentionFactorEncryptionToken::deriveFrom(serverDerivedFromDataToken)));
    ASSERT_EQ(counterPair.counter, counter);
    ASSERT_EQ(counterPair.contentionFactor, contention);

    ASSERT_TRUE(areBuffersEqual(mblock.getView().tag, tag));
}

TEST_F(ServiceContextTest, FLE_EDC_ServerSide_Equality_Payloads_V2_InvalidArgs) {
    TestKeyVault keyVault;
    auto emptyValue = ConstDataRange(0, 0);
    PrfBlock bogusTag;
    FLE2InsertUpdatePayloadV2 iupayload;
    auto bogusEncryptedTokens = StateCollectionTokensV2({{}}, false, boost::none).encrypt({{}});

    iupayload.setType(stdx::to_underlying(BSONType::numberLong));
    iupayload.setContentionFactor(0);
    iupayload.setIndexKeyId(indexKeyId);
    iupayload.setEdcDerivedToken({{}});
    iupayload.setEscDerivedToken({{}});
    iupayload.setServerEncryptionToken({{}});
    iupayload.setServerDerivedFromDataToken({{}});
    iupayload.setEncryptedTokens(bogusEncryptedTokens);

    // Test bad BSON Type
    iupayload.setType(-9);
    ASSERT_THROWS_CODE(FLE2IndexedEqualityEncryptedValueV2::fromUnencrypted(iupayload, bogusTag, 0),
                       DBException,
                       9697301);

    // Test unsupported BSON type for equality
    iupayload.setType(stdx::to_underlying(BSONType::maxKey));
    ASSERT_THROWS_CODE(FLE2IndexedEqualityEncryptedValueV2::fromUnencrypted(iupayload, bogusTag, 0),
                       DBException,
                       7291906);
    iupayload.setType(stdx::to_underlying(BSONType::numberLong));

    // Test empty client encrypted value
    iupayload.setValue(emptyValue);
    ASSERT_THROWS_CODE(FLE2IndexedEqualityEncryptedValueV2::fromUnencrypted(iupayload, bogusTag, 0),
                       DBException,
                       9697302);
}

TEST_F(ServiceContextTest, FLE_EDC_ServerSide_Payloads_V2_ParseInvalidInput) {
    ConstDataRange empty(0, 0);
    auto serverToken = ServerDataEncryptionLevel1Token::deriveFrom(getIndexKey());
    ServerDerivedFromDataToken serverDataDerivedToken(serverToken.asPrfBlock());

    // cipherTextSize minimum enforced by libmongocrypt is 17 (16 byte IV + 1 byte CTR)
    constexpr size_t minCipherTextLen = 17;
    constexpr size_t mblockSize = sizeof(FLE2TagAndEncryptedMetadataBlock::SerializedBlob);
    constexpr size_t typeOffset = 1 + UUID::kNumBytes;
    constexpr size_t edgeCountOffset = typeOffset + 1;

    std::vector<uint8_t> equalityInput(1 + UUID::kNumBytes + 1 + minCipherTextLen + mblockSize);
    equalityInput.at(0) = static_cast<uint8_t>(EncryptedBinDataType::kFLE2EqualityIndexedValueV2);
    std::vector<uint8_t> rangeInput(1 + UUID::kNumBytes + 1 + 1 + minCipherTextLen + mblockSize);
    rangeInput.at(0) = static_cast<uint8_t>(EncryptedBinDataType::kFLE2RangeIndexedValueV2);
    rangeInput.at(edgeCountOffset) = 1;

    // first, test that the shortest valid buffers can be parsed successfully
    ASSERT_DOES_NOT_THROW(FLE2IndexedEqualityEncryptedValueV2{equalityInput});
    ASSERT_DOES_NOT_THROW(FLE2IndexedRangeEncryptedValueV2{rangeInput});

    // test parsing equality as range fails
    ASSERT_THROWS_CODE(FLE2IndexedRangeEncryptedValueV2{equalityInput}, DBException, 9588706);

    // test parsing range as equality fails
    ASSERT_THROWS_CODE(FLE2IndexedEqualityEncryptedValueV2{rangeInput}, DBException, 9588708);

    std::vector<size_t> truncatedLengths = {
        0, 1, 10, typeOffset, /*edgeCountOffset, edgeCountOffset + 1,
        edgeCountOffset + 1 + mblockSize*/};

    // test short inputs for equality payload
    for (auto& testLength : truncatedLengths) {
        std::vector<uint8_t> testBuf(testLength);
        std::copy(equalityInput.begin(), equalityInput.begin() + testLength, testBuf.begin());
        ASSERT_THROWS_CODE(FLE2IndexedEqualityEncryptedValueV2{testBuf},
                           DBException,
                           ErrorCodes::LibmongocryptError);
    }

    // test short inputs for range payload
    for (auto& testLength : truncatedLengths) {
        std::vector<uint8_t> testBuf(testLength);
        std::copy(rangeInput.begin(), rangeInput.begin() + testLength, testBuf.begin());
        ASSERT_THROWS_CODE(
            FLE2IndexedRangeEncryptedValueV2{testBuf}, DBException, ErrorCodes::LibmongocryptError);
    }
}

// Test correct encryption/decryption of metadata block
TEST_F(ServiceContextTest, FLE_EDC_ServerSide_FLE2TagAndEncryptedMetadataBlock_RoundTrip) {
    const ServerDerivedFromDataToken token(
        decodePrf("986F23F132FF7F14F748AC69373CFC982AD0AD4BAD25BE92008B83AB43E96029"));
    const PrfBlock tag =
        decodePrf("BD53ACAC665EDD01E0CA30CB648B2B8F4967544047FD4E7D12B1A9BF07339928");
    const uint64_t count = 65;
    const uint64_t contention = 2;

    FLE2TagAndEncryptedMetadataBlock mblock;

    // Trying to get a view of a mblock before encryptAndSerialize throws.
    ASSERT_THROWS(mblock.getView(), DBException);

    ASSERT_OK(mblock.encryptAndSerialize(token, count, contention, tag));

    auto zerosToken = ServerZerosEncryptionToken::deriveFrom(token);
    auto countToken = ServerCountAndContentionFactorEncryptionToken::deriveFrom(token);
    auto view = mblock.getView();

    auto swZeros = mblock.decryptZerosBlob(zerosToken);
    ASSERT_OK(swZeros.getStatus());
    ASSERT_TRUE(FLE2TagAndEncryptedMetadataBlock::isValidZerosBlob(swZeros.getValue()));

    auto swPair = mblock.decryptCounterAndContentionFactorPair(countToken);
    ASSERT_OK(swPair.getStatus());
    ASSERT_EQ(count, swPair.getValue().counter);
    ASSERT_EQ(contention, swPair.getValue().contentionFactor);

    ASSERT_TRUE(areBuffersEqual(view.tag, tag));
}

TEST_F(ServiceContextTest, FLE_EDC_ServerSide_FLE2TagAndEncryptedMetadataBlock_IsValidZerosBlob) {
    FLE2TagAndEncryptedMetadataBlock::ZerosBlob zeros;
    zeros.fill(0);
    ASSERT_TRUE(FLE2TagAndEncryptedMetadataBlock::isValidZerosBlob(zeros));

    zeros[1] = 1;
    ASSERT_FALSE(FLE2TagAndEncryptedMetadataBlock::isValidZerosBlob(zeros));
}

TEST_F(ServiceContextTest, FLE_EDC_ServerSide_Range_Payloads_V2) {
    TestKeyVault keyVault;

    auto doc = BSON("sample" << 3);
    auto element = doc.firstElement();

    auto value = ConstDataRange(element.value(), element.value() + element.valuesize());

    auto collectionToken = CollectionsLevel1Token::deriveFrom(getIndexKey());
    auto serverEncryptToken = ServerDataEncryptionLevel1Token::deriveFrom(getIndexKey());
    auto serverDerivationToken = ServerTokenDerivationLevel1Token::deriveFrom(getIndexKey());

    auto edcToken = EDCToken::deriveFrom(collectionToken);
    auto escToken = ESCToken::deriveFrom(collectionToken);
    auto ecocToken = ECOCToken::deriveFrom(collectionToken);
    auto serverDerivedFromDataToken =
        ServerDerivedFromDataToken::deriveFrom(serverDerivationToken, value);
    FLECounter contention = 3;

    EDCDerivedFromDataToken edcDatakey = EDCDerivedFromDataToken::deriveFrom(edcToken, value);
    ESCDerivedFromDataToken escDatakey = ESCDerivedFromDataToken::deriveFrom(escToken, value);

    ESCDerivedFromDataTokenAndContentionFactorToken escDataCounterkey =
        ESCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(escDatakey, contention);
    EDCDerivedFromDataTokenAndContentionFactorToken edcDataCounterkey =
        EDCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(edcDatakey, contention);

    FLE2InsertUpdatePayloadV2 iupayload;
    iupayload.setEdcDerivedToken(edcDataCounterkey);
    iupayload.setEscDerivedToken(escDataCounterkey);
    iupayload.setServerEncryptionToken(serverEncryptToken);
    iupayload.setServerDerivedFromDataToken(serverDerivedFromDataToken);

    auto encryptedTokens =
        StateCollectionTokensV2(escDataCounterkey, false /* isLeaf */, boost::none)
            .encrypt(ecocToken);
    ASSERT_EQ(encryptedTokens.toCDR().length(), crypto::aesCTRIVSize + sizeof(PrfBlock) + 1);
    iupayload.setEncryptedTokens(encryptedTokens);
    iupayload.setIndexKeyId(indexKeyId);

    iupayload.setValue(value);
    iupayload.setType(stdx::to_underlying(element.type()));

    iupayload.setContentionFactor(contention);

    std::vector<EdgeTokenSetV2> tokens;
    EdgeTokenSetV2 ets;
    ets.setEdcDerivedToken(edcDataCounterkey);
    ets.setEscDerivedToken(escDataCounterkey);
    ets.setServerDerivedFromDataToken(serverDerivedFromDataToken);
    ets.setEncryptedTokens(encryptedTokens);

    tokens.push_back(ets);
    tokens.push_back(ets);

    iupayload.setEdgeTokenSet(tokens);

    auto edcTwiceDerived = EDCTwiceDerivedToken::deriveFrom(edcDataCounterkey);

    std::vector<uint64_t> counters = {123456, 456789};
    std::vector<PrfBlock> tags;
    std::transform(counters.begin(), counters.end(), std::back_inserter(tags), [&](uint64_t ct) {
        return EDCServerCollection::generateTag(&hmacCtx, edcTwiceDerived, ct);
    });

    // serialize to on-disk format
    auto serverPayload =
        FLE2IndexedRangeEncryptedValueV2::fromUnencrypted(iupayload, tags, counters);
    auto buf = uassertStatusOK(serverPayload.serialize());

    const size_t expectedSize = 1 + UUID::kNumBytes + 1 + sizeof(uint8_t) +
        (crypto::aesCTRIVSize + iupayload.getValue().length()) +
        (tags.size() * sizeof(FLE2TagAndEncryptedMetadataBlock::SerializedBlob));
    ASSERT_EQ(buf.size(), expectedSize);

    // parse it back
    FLE2IndexedRangeEncryptedValueV2 parsed(buf);

    // validate the original & parsed objects have identical values
    ASSERT_EQ(serverPayload, parsed);

    // validate individual fields have expected values
    ASSERT_EQ(parsed.getKeyId(), indexKeyId);
    ASSERT_EQ(parsed.getBsonType(), element.type());
    ASSERT_EQ(parsed.getTagCount(), tags.size());

    // verify the server-encrypted value decrypts back to client-encrypted value
    auto clientEncryptedValue = uassertStatusOK(
        FLEUtil::decryptData(serverEncryptToken.toCDR(), parsed.getServerEncryptedValue()));
    ASSERT_EQ(clientEncryptedValue.size(), value.length());
    ASSERT(std::equal(
        clientEncryptedValue.begin(), clientEncryptedValue.end(), value.data<uint8_t>()));

    // verify the metadata blocks have the correct values after decrypt
    auto mblocks = parsed.getMetadataBlocks();
    ASSERT_EQ(mblocks.size(), tags.size());
    auto zeroEncryptToken = ServerZerosEncryptionToken::deriveFrom(serverDerivedFromDataToken);
    auto counterEncryptToken =
        ServerCountAndContentionFactorEncryptionToken::deriveFrom(serverDerivedFromDataToken);

    for (size_t i = 0; i < mblocks.size(); i++) {
        auto zeros = uassertStatusOK(mblocks[i].decryptZerosBlob(zeroEncryptToken));
        ASSERT_TRUE(FLE2TagAndEncryptedMetadataBlock::isValidZerosBlob(zeros));

        auto counterPair =
            uassertStatusOK(mblocks[i].decryptCounterAndContentionFactorPair(counterEncryptToken));
        ASSERT_EQ(counterPair.counter, counters[i]);
        ASSERT_EQ(counterPair.contentionFactor, contention);

        ASSERT_TRUE(areBuffersEqual(mblocks[i].getView().tag, tags[i]));
    }
}

TEST_F(ServiceContextTest, FLE_EDC_ServerSide_Range_Payloads_V2_InvalidArgs) {
    TestKeyVault keyVault;
    auto doc = BSON("sample" << 3);
    auto element = doc.firstElement();
    auto value = ConstDataRange(element.value(), element.value() + element.valuesize());

    FLE2InsertUpdatePayloadV2 iupayload;

    iupayload.setValue(value);
    iupayload.setType(stdx::to_underlying(BSONType::numberLong));
    iupayload.setContentionFactor(0);
    iupayload.setIndexKeyId(indexKeyId);
    iupayload.setEdcDerivedToken({{}});
    iupayload.setEscDerivedToken({{}});
    iupayload.setServerEncryptionToken({{}});
    iupayload.setServerDerivedFromDataToken({{}});

    auto fakeEncryptedTokens = StateCollectionTokensV2({{}}, false, boost::none).encrypt({{}});
    iupayload.setEncryptedTokens(fakeEncryptedTokens);
    PrfBlock fakeTag;

    // test no edge token set in iupayload
    {
        ASSERT_THROWS_CODE(
            FLE2IndexedRangeEncryptedValueV2::fromUnencrypted(iupayload, {fakeTag}, {0}),
            DBException,
            9588700);
    }

    std::vector<EdgeTokenSetV2> tokens;
    EdgeTokenSetV2 ets;
    ets.setEdcDerivedToken({{}});
    ets.setEscDerivedToken({{}});
    ets.setServerDerivedFromDataToken({{}});
    ets.setEncryptedTokens(fakeEncryptedTokens);
    tokens.push_back(ets);
    iupayload.setEdgeTokenSet(tokens);

    // test bad bson type in iupayload
    {
        iupayload.setType(255);
        ASSERT_THROWS_CODE(
            FLE2IndexedRangeEncryptedValueV2::fromUnencrypted(iupayload, {fakeTag}, {0}),
            DBException,
            9588701);

        iupayload.setType(stdx::to_underlying(BSONType::string));
        ASSERT_THROWS_CODE(
            FLE2IndexedRangeEncryptedValueV2::fromUnencrypted(iupayload, {fakeTag}, {0}),
            DBException,
            7291908);
        iupayload.setType(stdx::to_underlying(BSONType::numberLong));
    }

    // test tags and counters vectors don't match the edge count
    {
        ASSERT_THROWS_CODE(
            FLE2IndexedRangeEncryptedValueV2::fromUnencrypted(iupayload, {fakeTag, fakeTag}, {0}),
            DBException,
            9588703);
        ASSERT_THROWS_CODE(
            FLE2IndexedRangeEncryptedValueV2::fromUnencrypted(iupayload, {fakeTag}, {0, 1}),
            DBException,
            9588704);
    }

    // test empty client ciphertext
    {
        iupayload.setValue(ConstDataRange{0, 0});
        ASSERT_THROWS_CODE(
            FLE2IndexedRangeEncryptedValueV2::fromUnencrypted(iupayload, {fakeTag}, {0}),
            DBException,
            9588705);
        iupayload.setValue(value);
    }

    // test edge count over limit
    {
        std::vector<EdgeTokenSetV2> t(EncryptionInformationHelpers::kFLE2PerFieldTagLimit + 1);
        std::vector<PrfBlock> tags(EncryptionInformationHelpers::kFLE2PerFieldTagLimit + 1);
        std::vector<uint64_t> counts(EncryptionInformationHelpers::kFLE2PerFieldTagLimit + 1);
        std::for_each(t.begin(), t.end(), [&](EdgeTokenSetV2 ets) {
            ets.setEdcDerivedToken({{}});
            ets.setEscDerivedToken({{}});
            ets.setServerDerivedFromDataToken({{}});
            ets.setEncryptedTokens(fakeEncryptedTokens);
        });
        iupayload.setEdgeTokenSet(t);

        ASSERT_THROWS_CODE(
            FLE2IndexedRangeEncryptedValueV2::fromUnencrypted(iupayload, tags, counts),
            DBException,
            9588702);
    }
}

static FLE2InsertUpdatePayloadV2 generateTestIUPV2ForTextSearch(BSONElement element) {
    FLE2InsertUpdatePayloadV2 iupayload;
    const FLECounter contention = 0;
    auto value = ConstDataRange(element.value(), element.value() + element.valuesize());

    auto collectionToken = CollectionsLevel1Token::deriveFrom(getIndexKey());
    auto serverEncryptToken = ServerDataEncryptionLevel1Token::deriveFrom(getIndexKey());
    auto serverDerivationToken = ServerTokenDerivationLevel1Token::deriveFrom(getIndexKey());
    auto edcToken = EDCToken::deriveFrom(collectionToken);
    auto escToken = ESCToken::deriveFrom(collectionToken);
    auto ecocToken = ECOCToken::deriveFrom(collectionToken);

    PrfBlock nullBlock;
    // d, s, l, p are bogus data
    iupayload.setEdcDerivedToken(EDCDerivedFromDataTokenAndContentionFactorToken(nullBlock));
    iupayload.setEscDerivedToken(ESCDerivedFromDataTokenAndContentionFactorToken(nullBlock));
    iupayload.setServerDerivedFromDataToken(ServerDerivedFromDataToken(nullBlock));
    iupayload.setEncryptedTokens(
        StateCollectionTokensV2(iupayload.getEscDerivedToken(), boost::none, 0).encrypt(ecocToken));
    // u, t, v, e, k
    iupayload.setIndexKeyId(indexKeyId);
    iupayload.setType(stdx::to_underlying(element.type()));
    iupayload.setValue(value);
    iupayload.setServerEncryptionToken(serverEncryptToken);
    iupayload.setContentionFactor(contention);
    // b
    iupayload.setTextSearchTokenSets(TextSearchTokenSets{{}, {}, {}, {}});
    auto& tsts = iupayload.getTextSearchTokenSets().value();
    {
        auto& exact = tsts.getExactTokenSet();
        auto edcDataDerivedToken = EDCTextExactDerivedFromDataToken::deriveFrom(
            EDCTextExactToken::deriveFrom(edcToken), value);
        auto escDataDerivedToken = ESCTextExactDerivedFromDataToken::deriveFrom(
            ESCTextExactToken::deriveFrom(escToken), value);
        exact.setEdcDerivedToken(
            EDCTextExactDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
                edcDataDerivedToken, contention));
        exact.setEscDerivedToken(
            ESCTextExactDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
                escDataDerivedToken, contention));
        exact.setServerDerivedFromDataToken(ServerTextExactDerivedFromDataToken::deriveFrom(
            ServerTextExactToken::deriveFrom(serverDerivationToken), value));
        exact.setEncryptedTokens(
            StateCollectionTokensV2(ESCDerivedFromDataTokenAndContentionFactorToken(
                                        exact.getEscDerivedToken().asPrfBlock()),
                                    boost::none,
                                    1 /* msize */)
                .encrypt(ecocToken));
    }
    return iupayload;
}

static void generateTextTokenSetsForIUPV2(FLE2InsertUpdatePayloadV2& iupayload,
                                          const std::vector<StringData>& strings,
                                          QueryTypeEnum type,
                                          uint32_t contention = 0) {
    if (!iupayload.getTextSearchTokenSets().has_value()) {
        iupayload.setTextSearchTokenSets(TextSearchTokenSets{{}, {}, {}, {}});
    }
    auto& tsts = iupayload.getTextSearchTokenSets().value();

    auto collectionToken = CollectionsLevel1Token::deriveFrom(getIndexKey());
    auto serverEncryptToken = ServerDataEncryptionLevel1Token::deriveFrom(getIndexKey());
    auto serverDerivationToken = ServerTokenDerivationLevel1Token::deriveFrom(getIndexKey());
    auto edcToken = EDCToken::deriveFrom(collectionToken);
    auto escToken = ESCToken::deriveFrom(collectionToken);
    auto ecocToken = ECOCToken::deriveFrom(collectionToken);

    for (const auto& str : strings) {
        auto doc = BSON("" << str);
        auto element = doc.firstElement();
        auto value = ConstDataRange(element.value(), element.value() + element.valuesize());

        if (type == QueryTypeEnum::SubstringPreview) {
            tsts.getSubstringTokenSets().push_back({});
            auto& ts = tsts.getSubstringTokenSets().back();
            auto edcDataDerivedToken = EDCTextSubstringDerivedFromDataToken::deriveFrom(
                EDCTextSubstringToken::deriveFrom(edcToken), value);
            auto escDataDerivedToken = ESCTextSubstringDerivedFromDataToken::deriveFrom(
                ESCTextSubstringToken::deriveFrom(escToken), value);

            ts.setEdcDerivedToken(
                EDCTextSubstringDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
                    edcDataDerivedToken, contention));
            ts.setEscDerivedToken(
                ESCTextSubstringDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
                    escDataDerivedToken, contention));
            ts.setServerDerivedFromDataToken(ServerTextSubstringDerivedFromDataToken::deriveFrom(
                ServerTextSubstringToken::deriveFrom(serverDerivationToken), value));
            ts.setEncryptedTokens(
                StateCollectionTokensV2(ESCDerivedFromDataTokenAndContentionFactorToken(
                                            ts.getEscDerivedToken().asPrfBlock()),
                                        boost::none,
                                        0 /* msize */)
                    .encrypt(ecocToken));
        } else if (type == QueryTypeEnum::SuffixPreview) {
            tsts.getSuffixTokenSets().push_back({});
            auto& ts = tsts.getSuffixTokenSets().back();
            auto edcDataDerivedToken = EDCTextSuffixDerivedFromDataToken::deriveFrom(
                EDCTextSuffixToken::deriveFrom(edcToken), value);
            auto escDataDerivedToken = ESCTextSuffixDerivedFromDataToken::deriveFrom(
                ESCTextSuffixToken::deriveFrom(escToken), value);

            ts.setEdcDerivedToken(
                EDCTextSuffixDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
                    edcDataDerivedToken, contention));
            ts.setEscDerivedToken(
                ESCTextSuffixDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
                    escDataDerivedToken, contention));
            ts.setServerDerivedFromDataToken(ServerTextSuffixDerivedFromDataToken::deriveFrom(
                ServerTextSuffixToken::deriveFrom(serverDerivationToken), value));
            ts.setEncryptedTokens(
                StateCollectionTokensV2(ESCDerivedFromDataTokenAndContentionFactorToken(
                                            ts.getEscDerivedToken().asPrfBlock()),
                                        boost::none,
                                        0 /* msize */)
                    .encrypt(ecocToken));
        } else if (type == QueryTypeEnum::PrefixPreview) {
            tsts.getPrefixTokenSets().push_back({});
            auto& ts = tsts.getPrefixTokenSets().back();
            auto edcDataDerivedToken = EDCTextPrefixDerivedFromDataToken::deriveFrom(
                EDCTextPrefixToken::deriveFrom(edcToken), value);
            auto escDataDerivedToken = ESCTextPrefixDerivedFromDataToken::deriveFrom(
                ESCTextPrefixToken::deriveFrom(escToken), value);

            ts.setEdcDerivedToken(
                EDCTextPrefixDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
                    edcDataDerivedToken, contention));
            ts.setEscDerivedToken(
                ESCTextPrefixDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
                    escDataDerivedToken, contention));
            ts.setServerDerivedFromDataToken(ServerTextPrefixDerivedFromDataToken::deriveFrom(
                ServerTextPrefixToken::deriveFrom(serverDerivationToken), value));
            ts.setEncryptedTokens(
                StateCollectionTokensV2(ESCDerivedFromDataTokenAndContentionFactorToken(
                                            ts.getEscDerivedToken().asPrfBlock()),
                                        boost::none,
                                        0 /* msize */)
                    .encrypt(ecocToken));
        }
    }
}

static std::vector<ServerDerivedFromDataToken> collectServerDerivedFromDataTokensForTextSearch(
    FLE2InsertUpdatePayloadV2& iupayload) {
    std::vector<ServerDerivedFromDataToken> tokens;
    if (!iupayload.getTextSearchTokenSets().has_value()) {
        return tokens;
    }
    tokens.emplace_back(iupayload.getTextSearchTokenSets()
                            ->getExactTokenSet()
                            .getServerDerivedFromDataToken()
                            .asPrfBlock());
    for (auto& ts : iupayload.getTextSearchTokenSets()->getSubstringTokenSets()) {
        tokens.emplace_back(ts.getServerDerivedFromDataToken().asPrfBlock());
    }
    for (auto& ts : iupayload.getTextSearchTokenSets()->getSuffixTokenSets()) {
        tokens.emplace_back(ts.getServerDerivedFromDataToken().asPrfBlock());
    }
    for (auto& ts : iupayload.getTextSearchTokenSets()->getPrefixTokenSets()) {
        tokens.emplace_back(ts.getServerDerivedFromDataToken().asPrfBlock());
    }
    return tokens;
}

// Tests round trip unparse/parse of FLE2IndexedTextEncryptedValue
TEST_F(ServiceContextTest, FLE_EDC_ServerSide_TextSearch_Payloads) {
    auto doc = BSON("sample" << "ssssssssss");

    EDCServerPayloadInfo payload;
    auto& iupayload = payload.payload = generateTestIUPV2ForTextSearch(doc.firstElement());

    std::vector<StringData> substrs = {"s", "ss", "sss", "ssss"};
    std::vector<StringData> suffixes = {"s", "ss"};
    std::vector<StringData> prefixes = {"s", "ss", "sss"};

    // Append fake padding strings > 255 to ensure tag counts over 8 bits long are ok.
    for (int i = 1; i <= 300; i++) {
        substrs.push_back("fake"_sd);
        suffixes.push_back("fake"_sd);
        prefixes.push_back("fake"_sd);
    }

    const size_t tagCount = 1 + substrs.size() + suffixes.size() + prefixes.size();

    generateTextTokenSetsForIUPV2(iupayload, substrs, QueryTypeEnum::SubstringPreview);
    generateTextTokenSetsForIUPV2(iupayload, suffixes, QueryTypeEnum::SuffixPreview);
    generateTextTokenSetsForIUPV2(iupayload, prefixes, QueryTypeEnum::PrefixPreview);

    payload.counts = std::vector<uint64_t>(tagCount);
    std::iota(payload.counts.begin(), payload.counts.end(), 1);  // fill with values 1...tagCount

    std::vector<PrfBlock> tags = EDCServerCollection::generateTagsForTextSearch(payload);
    ASSERT_EQ(tags.size(), tagCount);

    // serialize to on-disk format
    auto serverPayload =
        FLE2IndexedTextEncryptedValue::fromUnencrypted(iupayload, tags, payload.counts);
    auto buf = uassertStatusOK(serverPayload.serialize());

    const size_t expectedSize = 1 + UUID::kNumBytes + 1 + (3 * sizeof(uint32_t)) +
        (crypto::aesCTRIVSize + iupayload.getValue().length()) +
        (tagCount * sizeof(FLE2TagAndEncryptedMetadataBlock::SerializedBlob));
    ASSERT_EQ(buf.size(), expectedSize);

    // parse it back
    FLE2IndexedTextEncryptedValue parsed(buf);

    // validate the original & parsed objects have identical values
    ASSERT_EQ(serverPayload, parsed);

    ASSERT_EQ(stdx::to_underlying(parsed.getBsonType()), iupayload.getType());
    ASSERT_EQ(parsed.getKeyId(), iupayload.getIndexKeyId());
    ASSERT_EQ(parsed.getTagCount(), tagCount);
    ASSERT_EQ(parsed.getSubstringTagCount(), substrs.size());
    ASSERT_EQ(parsed.getSuffixTagCount(), suffixes.size());
    ASSERT_EQ(parsed.getPrefixTagCount(), prefixes.size());

    // verify the server-encrypted value decrypts back to client-encrypted value
    auto serverEncryptToken = ServerDataEncryptionLevel1Token::deriveFrom(getIndexKey());
    auto swValue =
        FLEUtil::decryptData(serverEncryptToken.toCDR(), parsed.getServerEncryptedValue());
    ASSERT(swValue.isOK());
    ASSERT_EQ(swValue.getValue().size(), iupayload.getValue().length());
    ASSERT(std::equal(swValue.getValue().begin(),
                      swValue.getValue().end(),
                      iupayload.getValue().data<uint8_t>()));

    auto serverDataTokens = collectServerDerivedFromDataTokensForTextSearch(iupayload);
    ASSERT_EQ(serverDataTokens.size(), parsed.getTagCount());

    auto substrBlks = parsed.getSubstringMetadataBlocks();
    auto suffixBlks = parsed.getSuffixMetadataBlocks();
    auto prefixBlks = parsed.getPrefixMetadataBlocks();
    ASSERT_EQ(substrBlks.size(), substrs.size());
    ASSERT_EQ(suffixBlks.size(), suffixes.size());
    ASSERT_EQ(prefixBlks.size(), prefixes.size());

    auto mblocks = parsed.getAllMetadataBlocks();

    // verify metadata blocks have the correct tags
    for (uint32_t i = 0; i < parsed.getTagCount(); i++) {
        auto zeroEncryptToken = ServerZerosEncryptionToken::deriveFrom(serverDataTokens[i]);
        auto counterEncryptToken =
            ServerCountAndContentionFactorEncryptionToken::deriveFrom(serverDataTokens[i]);

        auto zeros = uassertStatusOK(mblocks[i].decryptZerosBlob(zeroEncryptToken));
        ASSERT_TRUE(FLE2TagAndEncryptedMetadataBlock::isValidZerosBlob(zeros));

        auto counterPair =
            uassertStatusOK(mblocks[i].decryptCounterAndContentionFactorPair(counterEncryptToken));
        ASSERT_EQ(counterPair.counter, payload.counts[i]);
        ASSERT_EQ(counterPair.contentionFactor, 0);

        ASSERT_TRUE(areBuffersEqual(mblocks[i].getView().tag, tags[i]));
    }

    // Verify the metadata blocks have unique encrypted zeros blob
    // This ensures the AES-CTR IV is randomly set for blocks that use the same keys
    // like text search padding blocks.
    stdx::unordered_set<std::string> encryptedZeroesSet;
    for (auto& mblock : mblocks) {
        ASSERT(encryptedZeroesSet.insert(hexdump(mblock.getView().encryptedZeros)).second);
    }
}

TEST_F(ServiceContextTest, FLE_EDC_ServerSide_TextSearch_Payloads_EmptySubstringSuffixPrefixSets) {
    // test all of substring+suffix+prefix token sets empty
    auto doc = BSON("sample" << "ssssssssss");
    EDCServerPayloadInfo payload;
    auto& iupayload = payload.payload = generateTestIUPV2ForTextSearch(doc.firstElement());
    payload.counts = std::vector<uint64_t>(1);

    std::vector<PrfBlock> tags = EDCServerCollection::generateTagsForTextSearch(payload);
    ASSERT_EQ(tags.size(), 1);

    auto serverPayload =
        FLE2IndexedTextEncryptedValue::fromUnencrypted(iupayload, tags, payload.counts);
    auto buf = uassertStatusOK(serverPayload.serialize());
    FLE2IndexedTextEncryptedValue parsed(buf);
    ASSERT_EQ(parsed.getTagCount(), 1);
    ASSERT_EQ(parsed.getSubstringTagCount(), 0);
    ASSERT_EQ(parsed.getSuffixTagCount(), 0);
    ASSERT_EQ(parsed.getPrefixTagCount(), 0);
}

TEST_F(ServiceContextTest, FLE_EDC_ServerSide_TextSearch_Payloads_InvalidArgs) {
    auto doc = BSON("sample" << "ssssssssss");

    EDCServerPayloadInfo payload;
    auto& iupayload = payload.payload = generateTestIUPV2ForTextSearch(doc.firstElement());
    generateTextTokenSetsForIUPV2(
        iupayload, {"s", "ss", "sss", "ssss"}, QueryTypeEnum::SubstringPreview);
    payload.counts = std::vector<uint64_t>(5);

    std::vector<PrfBlock> tags = EDCServerCollection::generateTagsForTextSearch(payload);

    // test no text search token set in iupayload
    {
        auto tmpSets = iupayload.getTextSearchTokenSets();
        iupayload.setTextSearchTokenSets(boost::none);
        ASSERT_THROWS_CODE(
            FLE2IndexedTextEncryptedValue::fromUnencrypted(iupayload, tags, payload.counts),
            DBException,
            9784102);
        iupayload.setTextSearchTokenSets(std::move(tmpSets));
    }

    // test counters has wrong size
    {
        payload.counts.push_back(1);
        ASSERT_THROWS_CODE(
            FLE2IndexedTextEncryptedValue::fromUnencrypted(iupayload, tags, payload.counts),
            DBException,
            9784107);
        payload.counts.pop_back();
    }

    // test tags vector has wrong size
    {
        tags.push_back(PrfBlock{});
        ASSERT_THROWS_CODE(
            FLE2IndexedTextEncryptedValue::fromUnencrypted(iupayload, tags, payload.counts),
            DBException,
            9784113);
        tags.pop_back();
    }

    // test wrong BSON type
    {
        iupayload.setType(stdx::to_underlying(BSONType::numberInt));
        ASSERT_THROWS_CODE(
            FLE2IndexedTextEncryptedValue::fromUnencrypted(iupayload, tags, payload.counts),
            DBException,
            9784103);
        iupayload.setType(stdx::to_underlying(BSONType::string));
    }

    // test substr/suffix/prefix token sets too large
    {
        EDCServerPayloadInfo tmpPayload;
        tmpPayload.payload = generateTestIUPV2ForTextSearch(doc.firstElement());
        generateTextTokenSetsForIUPV2(tmpPayload.payload,
                                      std::vector<StringData>(84000, "s"_sd),
                                      QueryTypeEnum::SubstringPreview);
        tmpPayload.counts = std::vector<uint64_t>(84001);
        auto tmpTags = EDCServerCollection::generateTagsForTextSearch(tmpPayload);
        ASSERT_THROWS_CODE(FLE2IndexedTextEncryptedValue::fromUnencrypted(
                               tmpPayload.payload, tmpTags, tmpPayload.counts),
                           DBException,
                           9784104);
    }

    {
        EDCServerPayloadInfo tmpPayload;
        tmpPayload.payload = generateTestIUPV2ForTextSearch(doc.firstElement());
        generateTextTokenSetsForIUPV2(tmpPayload.payload,
                                      std::vector<StringData>(42000, "s"_sd),
                                      QueryTypeEnum::SubstringPreview);
        generateTextTokenSetsForIUPV2(tmpPayload.payload,
                                      std::vector<StringData>(42000, "s"_sd),
                                      QueryTypeEnum::SuffixPreview);
        tmpPayload.counts = std::vector<uint64_t>(84001);
        auto tmpTags = EDCServerCollection::generateTagsForTextSearch(tmpPayload);
        ASSERT_THROWS_CODE(FLE2IndexedTextEncryptedValue::fromUnencrypted(
                               tmpPayload.payload, tmpTags, tmpPayload.counts),
                           DBException,
                           9784105);
    }

    {
        EDCServerPayloadInfo tmpPayload;
        tmpPayload.payload = generateTestIUPV2ForTextSearch(doc.firstElement());
        generateTextTokenSetsForIUPV2(tmpPayload.payload,
                                      std::vector<StringData>(28000, "s"_sd),
                                      QueryTypeEnum::SubstringPreview);
        generateTextTokenSetsForIUPV2(tmpPayload.payload,
                                      std::vector<StringData>(28000, "s"_sd),
                                      QueryTypeEnum::SuffixPreview);
        generateTextTokenSetsForIUPV2(tmpPayload.payload,
                                      std::vector<StringData>(28000, "s"_sd),
                                      QueryTypeEnum::PrefixPreview);
        tmpPayload.counts = std::vector<uint64_t>(84001);
        auto tmpTags = EDCServerCollection::generateTagsForTextSearch(tmpPayload);
        ASSERT_THROWS_CODE(FLE2IndexedTextEncryptedValue::fromUnencrypted(
                               tmpPayload.payload, tmpTags, tmpPayload.counts),
                           DBException,
                           9784106);
    }
}

TEST_F(ServiceContextTest, FLE_EDC_DuplicateSafeContent_CompatibleType) {

    TestKeyVault keyVault;

    auto doc = BSON("value" << "123456");
    auto element = doc.firstElement();
    auto inputDoc = BSON(kSafeContent << BSON_ARRAY(1 << 2 << 4) << "encrypted" << element);

    auto buf = generatePlaceholder(element, Operation::kInsert);
    BSONObjBuilder builder;
    builder.append(kSafeContent, BSON_ARRAY(1 << 2 << 4));
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());

    auto finalDoc = encryptDocument(builder.obj(), &keyVault);

    ASSERT_EQ(finalDoc[kSafeContent].type(), BSONType::array);
    ASSERT_EQ(finalDoc["encrypted"].type(), BSONType::binData);
    ASSERT_TRUE(finalDoc["encrypted"].isBinData(BinDataType::Encrypt));

    // Decrypt document
    auto decryptedDoc = FLEClientCrypto::decryptDocument(finalDoc, &keyVault);

    std::cout << "Final Doc: " << decryptedDoc << std::endl;

    auto elements = finalDoc[kSafeContent].Array();
    ASSERT_EQ(elements.size(), 4);
    ASSERT_EQ(elements[0].safeNumberInt(), 1);
    ASSERT_EQ(elements[1].safeNumberInt(), 2);
    ASSERT_EQ(elements[2].safeNumberInt(), 4);
    ASSERT(elements[3].type() == BSONType::binData);
}


TEST_F(ServiceContextTest, FLE_EDC_DuplicateSafeContent_IncompatibleType) {

    TestKeyVault keyVault;

    auto doc = BSON("value" << "123456");
    auto element = doc.firstElement();

    auto buf = generatePlaceholder(element, Operation::kInsert);
    BSONObjBuilder builder;
    builder.append(kSafeContent, 123456);
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());

    ASSERT_THROWS_CODE(encryptDocument(builder.obj(), &keyVault), DBException, 6373510);
}

std::vector<char> generateRangeIntPlaceholder(BSONElement value,
                                              Operation operation,
                                              double indexMin,
                                              double indexMax,
                                              uint64_t contention,
                                              uint32_t trimFactor,
                                              uint32_t precision,
                                              uint32_t sparsity) {
    FLE2EncryptionPlaceholder ep;

    if (operation == Operation::kFind) {
        ep.setType(mongo::Fle2PlaceholderType::kFind);
    } else if (operation == Operation::kInsert) {
        ep.setType(mongo::Fle2PlaceholderType::kInsert);
    }

    ep.setAlgorithm(Fle2AlgorithmInt::kRange);
    ep.setUserKeyId(userKeyId);
    ep.setIndexKeyId(indexKeyId);
    ep.setSparsity(sparsity);

    FLE2RangeInsertSpec insertSpec;

    BSONObj lowerDoc = BSON("lb" << indexMin);
    BSONObj upperDoc = BSON("ub" << indexMax);
    insertSpec.setValue(value);
    insertSpec.setPrecision(precision);
    insertSpec.setTrimFactor(trimFactor);

    insertSpec.setMinBound(boost::optional<IDLAnyType>(lowerDoc.firstElement()));
    insertSpec.setMaxBound(boost::optional<IDLAnyType>(upperDoc.firstElement()));
    auto specDoc = BSON("s" << insertSpec.toBSON());

    FLE2RangeFindSpecEdgesInfo edgesInfo;
    FLE2RangeFindSpec findSpec;

    edgesInfo.setLowerBound(lowerDoc.firstElement());
    edgesInfo.setLbIncluded(true);
    edgesInfo.setUpperBound(upperDoc.firstElement());
    edgesInfo.setUbIncluded(true);

    BSONObj indexMinDoc = BSON("lb" << indexMin);
    BSONObj indexMaxDoc = BSON("ub" << indexMax);

    edgesInfo.setIndexMin(indexMinDoc.firstElement());
    edgesInfo.setIndexMax(indexMaxDoc.firstElement());
    edgesInfo.setTrimFactor(trimFactor);
    edgesInfo.setPrecision(precision);

    findSpec.setEdgesInfo(edgesInfo);

    findSpec.setFirstOperator(Fle2RangeOperator::kGt);

    findSpec.setPayloadId(1234);

    auto findDoc = BSON("s" << findSpec.toBSON());

    if (operation == Operation::kFind) {
        ep.setValue(IDLAnyType(findDoc.firstElement()));
    } else if (operation == Operation::kInsert) {
        ep.setValue(IDLAnyType(specDoc.firstElement()));
    }

    ep.setSparsity(sparsity);
    ep.setMaxContentionCounter(contention);

    BSONObj obj = ep.toBSON();

    std::vector<char> v;
    v.resize(obj.objsize() + 1);
    v[0] = static_cast<uint8_t>(EncryptedBinDataType::kFLE2Placeholder);
    std::copy(obj.objdata(), obj.objdata() + obj.objsize(), v.begin() + 1);
    return v;
}

TEST_F(ServiceContextTest, FLE_EDC_RangeParamtersFlow_Insert) {

    TestKeyVault keyVault;
    auto obj = BSON("encrypted" << 1.23);
    auto buf =
        generateRangeIntPlaceholder(obj.firstElement(), Operation::kInsert, 1, 42, 3, 5, 4, 2);

    BSONObjBuilder builder;
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());

    auto result = FLEClientCrypto::transformPlaceholders(builder.obj(), &keyVault);
    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);
    ASSERT_EQ(serverPayload.size(), 1);
    auto payload = serverPayload[0];
    ASSERT_TRUE(payload.isRangePayload());

    ASSERT_EQ(payload.payload.getPrecision().get(), 4);
    ASSERT_EQ(payload.payload.getTrimFactor().get(), 5);
    ASSERT_EQ(payload.payload.getSparsity().get(), 2);
    auto minExpected = BSON("mn" << 1.0);
    auto maxExpected = BSON("mx" << 42.0);
    ASSERT(
        payload.payload.getIndexMin().get().getElement().binaryEqual(minExpected.firstElement()));
    ASSERT(
        payload.payload.getIndexMax().get().getElement().binaryEqual(maxExpected.firstElement()));
}

TEST_F(ServiceContextTest, FLE_EDC_RangeParamtersFlow_Find) {

    TestKeyVault keyVault;
    auto obj = BSON("encrypted" << 123456.7);
    auto buf = generateRangeIntPlaceholder(obj.firstElement(), Operation::kFind, 1, 42, 3, 5, 4, 2);

    BSONObjBuilder builder;
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());

    auto result = FLEClientCrypto::transformPlaceholders(builder.obj(), &keyVault);

    auto rangePayload = ParsedFindRangePayload(result.firstElement());

    ASSERT_EQ(rangePayload.precision.get(), 4);
    ASSERT_EQ(rangePayload.trimFactor.get(), 5);
    ASSERT_EQ(rangePayload.sparsity.get(), 2);
    auto minExpected = BSON("mn" << 1.0);
    auto maxExpected = BSON("mx" << 42.0);
    ASSERT(rangePayload.indexMin.get().getElement().binaryEqual(minExpected.firstElement()));
    ASSERT(rangePayload.indexMax.get().getElement().binaryEqual(maxExpected.firstElement()));
}


TEST_F(ServiceContextTest, FLE_ECOC_EncryptedTokensRoundTrip) {
    std::vector<uint8_t> value(4);

    auto collectionToken = CollectionsLevel1Token::deriveFrom(getIndexKey());
    auto escToken = ESCToken::deriveFrom(collectionToken);
    auto ecocToken = ECOCToken::deriveFrom(collectionToken);
    auto escDataToken = ESCDerivedFromDataToken::deriveFrom(escToken, value);
    auto escContentionToken =
        ESCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(escDataToken, 1);

    std::vector<std::tuple<boost::optional<bool>, boost::optional<std::uint32_t>>> etMetaValues(
        {{boost::none, boost::none},
         {true, boost::none},
         {false, boost::none},
         {boost::none, 0},
         {boost::none, 1},
         {boost::none, 0xfffefd}});
    for (auto [optIsLeaf, optMsize] : etMetaValues) {
        StateCollectionTokensV2 encryptor{escContentionToken, optIsLeaf, optMsize};
        auto encryptedTokens = encryptor.encrypt(ecocToken);
        ASSERT_EQ(encryptedTokens.toCDR().length(),
                  crypto::aesCTRIVSize + sizeof(PrfBlock) + (optIsLeaf ? 1 : 0) +
                      (optMsize ? 3 : 0));

        auto decoded = encryptedTokens.decrypt(ecocToken);
        ASSERT_EQ(encryptor.getESCDerivedFromDataTokenAndContentionFactorToken(),
                  decoded.getESCDerivedFromDataTokenAndContentionFactorToken());
        ASSERT_EQ(encryptor.getIsLeaf(), decoded.getIsLeaf());
        ASSERT_EQ(encryptor.getMsize(), decoded.getMsize());

        auto rawEcocDoc = encryptedTokens.generateDocument("foo");

        auto ecocDoc = ECOCCompactionDocumentV2::parseAndDecrypt(rawEcocDoc, ecocToken);
        ASSERT_EQ(ecocDoc.fieldName, "foo");
        ASSERT_EQ(ecocDoc.esc, escContentionToken);
        ASSERT_EQ(ecocDoc.isLeaf, optIsLeaf);
        ASSERT_EQ(ecocDoc.msize, optMsize);
    }
}

template <typename T, typename Func>
bool vectorContains(const std::vector<T>& vec, Func func) {
    return std::find_if(vec.begin(), vec.end(), func) != vec.end();
}

TEST_F(ServiceContextTest, EncryptionInformation_RoundTrip) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest("test.test");

    EncryptedFieldConfig efc = getTestEncryptedFieldConfig();
    auto obj = EncryptionInformationHelpers::encryptionInformationSerialize(ns, efc);


    EncryptedFieldConfig efc2 = EncryptionInformationHelpers::getAndValidateSchema(
        ns, EncryptionInformation::parse(obj, IDLParserContext("foo")));

    ASSERT_BSONOBJ_EQ(efc.toBSON(), efc2.toBSON());
}

TEST_F(ServiceContextTest, EncryptionInformation_BadSchema) {
    EncryptionInformation ei;
    ei.setType(1);

    ei.setSchema(BSON("a" << "b"));

    auto obj = ei.toBSON();

    NamespaceString ns = NamespaceString::createNamespaceString_forTest("test.test");
    ASSERT_THROWS_CODE(EncryptionInformationHelpers::getAndValidateSchema(
                           ns, EncryptionInformation::parse(obj, IDLParserContext("foo"))),
                       DBException,
                       6371205);
}

TEST_F(ServiceContextTest, EncryptionInformation_MissingStateCollection) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest("test.test");

    {
        EncryptedFieldConfig efc = getTestEncryptedFieldConfig();
        efc.setEscCollection(boost::none);
        auto obj = EncryptionInformationHelpers::encryptionInformationSerialize(ns, efc);
        ASSERT_THROWS_CODE(EncryptionInformationHelpers::getAndValidateSchema(
                               ns, EncryptionInformation::parse(obj, IDLParserContext("foo"))),
                           DBException,
                           6371207);
    }
    {
        EncryptedFieldConfig efc = getTestEncryptedFieldConfig();
        efc.setEcocCollection(boost::none);
        auto obj = EncryptionInformationHelpers::encryptionInformationSerialize(ns, efc);
        ASSERT_THROWS_CODE(EncryptionInformationHelpers::getAndValidateSchema(
                               ns, EncryptionInformation::parse(obj, IDLParserContext("foo"))),
                           DBException,
                           6371208);
    }
}

TEST_F(ServiceContextTest, EncryptionInformation_TestTagLimitsForTextSearch) {
    auto makeTextQueryTypeConfig =
        [](QueryTypeEnum qtype, int32_t lb, int32_t ub, boost::optional<int32_t> mlen) {
            QueryTypeConfig qtc{qtype};
            qtc.setStrMinQueryLength(lb);
            qtc.setStrMaxQueryLength(ub);
            qtc.setStrMaxLength(std::move(mlen));
            return qtc;
        };
    auto assertExpectedMaxTags = [](const std::vector<QueryTypeConfig>& qtc, uint32_t expected) {
        uint32_t actual = 1;
        for (auto& qt : qtc) {
            auto lb = qt.getStrMinQueryLength().get();
            auto ub = qt.getStrMaxQueryLength().get();
            actual += ((qt.getQueryType() == QueryTypeEnum::SubstringPreview)
                           ? maxTagsForSubstring(lb, ub, qt.getStrMaxLength().get())
                           : maxTagsForSuffixOrPrefix(lb, ub));
        }
        ASSERT_EQ(actual, expected);
    };
    auto doOneFieldTest = [](const std::vector<QueryTypeConfig>& qtc, boost::optional<int> error) {
        EncryptedFieldConfig efc;
        EncryptedField field{UUID::gen(), "field"};
        field.setBsonType("string"_sd);
        field.setQueries(std::variant<std::vector<QueryTypeConfig>, QueryTypeConfig>{qtc});
        efc.setFields({field});
        if (error) {
            ASSERT_THROWS_CODE(
                EncryptionInformationHelpers::checkTagLimitsAndStorageNotExceeded(efc),
                DBException,
                *error);
        } else {
            ASSERT_DOES_NOT_THROW(
                EncryptionInformationHelpers::checkTagLimitsAndStorageNotExceeded(efc));
        }
    };

    // substring field under limit
    std::vector<QueryTypeConfig> qtc = {
        makeTextQueryTypeConfig(QueryTypeEnum::SubstringPreview, 10, 100, 900)};
    assertExpectedMaxTags(qtc, 76987);
    doOneFieldTest(qtc, {});

    // substring field at limit
    qtc.front() = makeTextQueryTypeConfig(QueryTypeEnum::SubstringPreview, 1, 1, 83'999);
    assertExpectedMaxTags(qtc, 84'000);
    doOneFieldTest(qtc, {});

    qtc.front() = makeTextQueryTypeConfig(QueryTypeEnum::SubstringPreview, 1, 2, 42'000);
    assertExpectedMaxTags(qtc, 84'000);
    doOneFieldTest(qtc, {});

    // substring field over limit
    qtc.front() = makeTextQueryTypeConfig(QueryTypeEnum::SubstringPreview, 10, 100, 1000);
    assertExpectedMaxTags(qtc, 86'087);
    doOneFieldTest(qtc, 10384602);

    // overflow uint32_t
    qtc.front() = makeTextQueryTypeConfig(QueryTypeEnum::SubstringPreview, 1, INT32_MAX, INT32_MAX);
    doOneFieldTest(qtc, 10384601);

    for (auto qtype : {QueryTypeEnum::SuffixPreview, QueryTypeEnum::PrefixPreview}) {
        // suffix/prefix field under limit
        qtc.front() = makeTextQueryTypeConfig(qtype, 9, 109, {});
        assertExpectedMaxTags(qtc, 102);
        doOneFieldTest(qtc, {});

        // suffix/prefix field at limit
        qtc.front() = makeTextQueryTypeConfig(qtype, 21, 84'019, {});
        assertExpectedMaxTags(qtc, 84'000);
        doOneFieldTest(qtc, {});

        // suffix/prefix field over limit
        qtc.front() = makeTextQueryTypeConfig(qtype, 1, 84'001, {});
        assertExpectedMaxTags(qtc, 84'002);
        doOneFieldTest(qtc, 10384602);
    }

    // suffix+prefix field under limit
    qtc = {makeTextQueryTypeConfig(QueryTypeEnum::SuffixPreview, 9, 109, {}),
           makeTextQueryTypeConfig(QueryTypeEnum::PrefixPreview, 90, 900, {})};
    assertExpectedMaxTags(qtc, 101 + 811 + 1);
    doOneFieldTest(qtc, {});

    // suffix+prefix field at limit
    qtc = {makeTextQueryTypeConfig(QueryTypeEnum::SuffixPreview, 101, 42100, {}),
           makeTextQueryTypeConfig(QueryTypeEnum::PrefixPreview, 1001, 42999, {})};
    assertExpectedMaxTags(qtc, 84'000);
    doOneFieldTest(qtc, {});

    // suffix+prefix field over limit
    qtc = {makeTextQueryTypeConfig(QueryTypeEnum::SuffixPreview, 101, 42100, {}),
           makeTextQueryTypeConfig(QueryTypeEnum::PrefixPreview, 1001, 43000, {})};
    assertExpectedMaxTags(qtc, 84'001);
    doOneFieldTest(qtc, 10384602);
}

TEST_F(ServiceContextTest, EncryptionInformation_TestTagStorageLimits) {
    auto makeEqualityQueryTypeConfig = []() {
        QueryTypeConfig config;
        config.setQueryType(QueryTypeEnum::Equality);

        return config;
    };
    auto makeRangeQueryTypeConfigInt32 =
        [](int32_t lb, int32_t ub, const boost::optional<uint32_t>& precision, int sparsity) {
            QueryTypeConfig config;
            config.setQueryType(QueryTypeEnum::Range);
            config.setMin(Value(lb));
            config.setMax(Value(ub));
            config.setSparsity(sparsity);
            if (precision) {
                config.setPrecision(*precision);
            }
            config.setTrimFactor(0);
            return config;
        };
    auto makeTextQueryTypeConfig =
        [](QueryTypeEnum qtype, int32_t lb, int32_t ub, boost::optional<int32_t> mlen) {
            QueryTypeConfig qtc{qtype};
            qtc.setStrMinQueryLength(lb);
            qtc.setStrMaxQueryLength(ub);
            qtc.setStrMaxLength(std::move(mlen));
            return qtc;
        };
    auto makeEncryptedFields =
        [](int numFields, StringData bsonType, const std::vector<QueryTypeConfig>& qtc) {
            std::vector<EncryptedField> fields;
            for (int i = 0; i < numFields; i++) {
                EncryptedField field{UUID::gen(), fmt::format("field_{}", i)};
                field.setBsonType(bsonType);
                field.setQueries(std::variant<std::vector<QueryTypeConfig>, QueryTypeConfig>{qtc});
                fields.emplace_back(field);
            }

            return fields;
        };
    auto doMultipleFieldsTest = [](const EncryptedFieldConfig& efc, boost::optional<int> error) {
        // Regardless of "error", we always expect success when the override cluster param is set.
        {
            FLEOverrideTagOverheadData tagOverheadOverride;
            tagOverheadOverride.setShouldOverride(true);

            RAIIServerParameterControllerForTest overrideTagOverheadLimit(
                "fleAllowTotalTagOverheadToExceedBSONLimit", tagOverheadOverride);
            ASSERT_DOES_NOT_THROW(
                EncryptionInformationHelpers::checkTagLimitsAndStorageNotExceeded(efc));
        }

        if (error) {
            ASSERT_THROWS_CODE(
                EncryptionInformationHelpers::checkTagLimitsAndStorageNotExceeded(efc),
                DBException,
                *error);
        } else {
            ASSERT_DOES_NOT_THROW(
                EncryptionInformationHelpers::checkTagLimitsAndStorageNotExceeded(efc));
        }
    };

    // Fields should fail if total tag storage exceeds BSON limit even while each field is
    // within tag limit

    // equality above total tag storage limit
    EncryptedFieldConfig efc;
    efc.setFields(makeEncryptedFields(131073, "string"_sd, {makeEqualityQueryTypeConfig()}));
    doMultipleFieldsTest(efc, 10431800);

    // equality within total tag storage limit
    efc.setFields(makeEncryptedFields(131072, "string"_sd, {makeEqualityQueryTypeConfig()}));
    doMultipleFieldsTest(efc, {});

    // range above total tag storage limit
    efc.setFields(makeEncryptedFields(
        11916, "int"_sd, {makeRangeQueryTypeConfigInt32(1, 1000, boost::none, 1)}));
    doMultipleFieldsTest(efc, 10431800);

    // range within total tag storage limit
    efc.setFields(makeEncryptedFields(
        11915, "int"_sd, {makeRangeQueryTypeConfigInt32(1, 1000, boost::none, 1)}));
    doMultipleFieldsTest(efc, {});

    // substring above total tag storage limit
    efc.setFields(makeEncryptedFields(
        37, "string"_sd, {makeTextQueryTypeConfig(QueryTypeEnum::SubstringPreview, 2, 10, 400)}));
    doMultipleFieldsTest(efc, 10431800);

    // substring within total tag storage limit
    efc.setFields(makeEncryptedFields(
        36, "string"_sd, {makeTextQueryTypeConfig(QueryTypeEnum::SubstringPreview, 2, 10, 400)}));
    doMultipleFieldsTest(efc, {});

    // prefix above total tag storage limit
    efc.setFields(makeEncryptedFields(
        1286, "string"_sd, {makeTextQueryTypeConfig(QueryTypeEnum::PrefixPreview, 9, 109, {})}));
    doMultipleFieldsTest(efc, 10431800);

    // prefix within total tag storage limit
    efc.setFields(makeEncryptedFields(
        1285, "string"_sd, {makeTextQueryTypeConfig(QueryTypeEnum::PrefixPreview, 9, 109, {})}));
    doMultipleFieldsTest(efc, {});

    // suffix above total tag storage limit
    efc.setFields(makeEncryptedFields(
        1286, "string"_sd, {makeTextQueryTypeConfig(QueryTypeEnum::SuffixPreview, 9, 109, {})}));
    doMultipleFieldsTest(efc, 10431800);

    // suffix within total tag storage limit
    efc.setFields(makeEncryptedFields(
        1285, "string"_sd, {makeTextQueryTypeConfig(QueryTypeEnum::SuffixPreview, 9, 109, {})}));
    doMultipleFieldsTest(efc, {});

    // suffix+prefix above total tag storage limit
    efc.setFields(
        makeEncryptedFields(144,
                            "string"_sd,
                            {makeTextQueryTypeConfig(QueryTypeEnum::PrefixPreview, 9, 109, {}),
                             makeTextQueryTypeConfig(QueryTypeEnum::SuffixPreview, 90, 900, {})}));
    doMultipleFieldsTest(efc, 10431800);

    // suffix+prefix within total tag storage limit
    efc.setFields(
        makeEncryptedFields(143,
                            "string"_sd,
                            {makeTextQueryTypeConfig(QueryTypeEnum::PrefixPreview, 9, 109, {}),
                             makeTextQueryTypeConfig(QueryTypeEnum::SuffixPreview, 90, 900, {})}));
    doMultipleFieldsTest(efc, {});

    // mixture of substring, suffix, and prefix above total tag storage limit
    std::vector<EncryptedField> fields;
    auto substringFields = makeEncryptedFields(
        36, "string"_sd, {makeTextQueryTypeConfig(QueryTypeEnum::SubstringPreview, 2, 10, 400)});
    auto prefixFields = makeEncryptedFields(
        15, "string"_sd, {makeTextQueryTypeConfig(QueryTypeEnum::PrefixPreview, 9, 109, {})});
    auto suffixFields = makeEncryptedFields(
        15, "string"_sd, {makeTextQueryTypeConfig(QueryTypeEnum::SuffixPreview, 9, 109, {})});
    fields.insert(fields.end(), substringFields.begin(), substringFields.end());
    fields.insert(fields.end(), prefixFields.begin(), prefixFields.end());
    fields.insert(fields.end(), suffixFields.begin(), suffixFields.end());
    efc.setFields(fields);
    doMultipleFieldsTest(efc, 10431800);

    // mixture of substring, suffix, and prefix within total tag storage limit
    suffixFields = makeEncryptedFields(
        14, "string"_sd, {makeTextQueryTypeConfig(QueryTypeEnum::SuffixPreview, 9, 109, {})});
    fields.clear();
    fields.insert(fields.end(), substringFields.begin(), substringFields.end());
    fields.insert(fields.end(), prefixFields.begin(), prefixFields.end());
    fields.insert(fields.end(), suffixFields.begin(), suffixFields.end());
    efc.setFields(fields);
    doMultipleFieldsTest(efc, {});

    // mixture of substring, equality, and range above total tag storage limit
    substringFields = makeEncryptedFields(
        14, "string"_sd, {makeTextQueryTypeConfig(QueryTypeEnum::SubstringPreview, 2, 10, 400)});
    auto equalityFields = makeEncryptedFields(6785, "string"_sd, {makeEqualityQueryTypeConfig()});
    auto rangeFields = makeEncryptedFields(
        6800, "int"_sd, {makeRangeQueryTypeConfigInt32(1, 1000, boost::none, 1)});
    fields.clear();
    fields.insert(fields.end(), substringFields.begin(), substringFields.end());
    fields.insert(fields.end(), equalityFields.begin(), equalityFields.end());
    fields.insert(fields.end(), rangeFields.begin(), rangeFields.end());
    efc.setFields(fields);
    doMultipleFieldsTest(efc, 10431800);

    // mixture of substring, equality, and range within total tag storage limit
    equalityFields = makeEncryptedFields(5000, "string"_sd, {makeEqualityQueryTypeConfig()});
    fields.clear();
    fields.insert(fields.end(), substringFields.begin(), substringFields.end());
    fields.insert(fields.end(), equalityFields.begin(), equalityFields.end());
    fields.insert(fields.end(), rangeFields.begin(), rangeFields.end());
    efc.setFields(fields);
    doMultipleFieldsTest(efc, {});
}

TEST_F(ServiceContextTest, EncryptionInformation_TestSubstringPreviewParameterLimits) {
    auto makeQueryTypeConfig = [](int32_t lb, int32_t ub, int32_t mlen) {
        QueryTypeConfig qtc{QueryTypeEnum::SubstringPreview};
        qtc.setStrMinQueryLength(lb);
        qtc.setStrMaxQueryLength(ub);
        qtc.setStrMaxLength(mlen);
        return qtc;
    };
    auto doOneFieldTest = [](const std::vector<QueryTypeConfig>& qtc, boost::optional<int> error) {
        EncryptedFieldConfig efc;
        EncryptedField field{UUID::gen(), "field"};
        field.setBsonType("string"_sd);
        field.setQueries(std::variant<std::vector<QueryTypeConfig>, QueryTypeConfig>{qtc});
        efc.setFields({field});
        if (error) {
            ASSERT_THROWS_CODE(
                EncryptionInformationHelpers::checkSubstringPreviewParameterLimitsNotExceeded(efc),
                DBException,
                *error);
        } else {
            ASSERT_DOES_NOT_THROW(
                EncryptionInformationHelpers::checkSubstringPreviewParameterLimitsNotExceeded(efc));
        }
    };

    // substring max length over upper limit
    std::vector<QueryTypeConfig> qtc = {makeQueryTypeConfig(2, 10, 900)};
    doOneFieldTest(qtc, 10453202);

    // substring max query length over upper limit
    qtc[0] = makeQueryTypeConfig(2, 60, 60);
    doOneFieldTest(qtc, 10453201);

    // substring min query length below lower limit
    qtc[0] = makeQueryTypeConfig(1, 10, 60);
    doOneFieldTest(qtc, 10453200);

    // substring params all within limtis
    qtc[0] = makeQueryTypeConfig(2, 10, 60);
    doOneFieldTest(qtc, {});
}

TEST_F(ServiceContextTest, IndexedFields_FetchTwoLevels) {
    TestKeyVault keyVault;

    auto doc = BSON("value" << "123456");
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
TEST_F(ServiceContextTest, IndexedFields_DuplicateIndexKeyIds) {
    TestKeyVault keyVault;

    auto doc = BSON("value" << "123456");
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
TEST_F(ServiceContextTest, TagDelta_Basic) {
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

TEST_F(ServiceContextTest, EDC_NonMatchingSchema) {
    EncryptedFieldConfig efc = getTestEncryptedFieldConfig();

    TestKeyVault keyVault;

    BSONObjBuilder builder;
    builder.append("plainText", "sample");
    auto doc = BSON("a" << "not really a secret");
    auto element = doc.firstElement();
    auto buf = generatePlaceholder(element, Operation::kInsert);
    builder.appendBinData("not-encrypted", buf.size(), BinDataType::Encrypt, buf.data());

    ASSERT_THROWS_CODE(encryptDocument(builder.obj(), &keyVault, &efc), DBException, 6373601);
}

TEST_F(ServiceContextTest, EDC_EncryptAlreadyEncryptedData) {
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

TEST_F(ServiceContextTest, FLE1_EncryptAlreadyEncryptedDataLegacy) {
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
TEST_F(ServiceContextTest, FLE_Update_Basic) {
    TestKeyVault keyVault;

    auto doc = BSON("value" << "123456");
    auto element = doc.firstElement();
    auto buf = generatePlaceholder(element, Operation::kInsert);
    auto inputDoc = BSON(
        "$set" << BSON("encrypted" << BSONBinData(buf.data(), buf.size(), BinDataType::Encrypt)));
    auto finalDoc = encryptUpdateDocument(inputDoc, &keyVault);

    std::cout << finalDoc << std::endl;

    ASSERT_TRUE(finalDoc["$set"]["encrypted"].isBinData(BinDataType::Encrypt));
    ASSERT_TRUE(finalDoc["$push"][kSafeContent]["$each"].type() == BSONType::array);
    ASSERT_EQ(finalDoc["$push"][kSafeContent]["$each"].Array().size(), 1);
    ASSERT_TRUE(
        finalDoc["$push"][kSafeContent]["$each"].Array()[0].isBinData(BinDataType::BinDataGeneral));
}

// Test update with no crypto
TEST_F(ServiceContextTest, FLE_Update_Empty) {
    TestKeyVault keyVault;

    auto inputDoc = BSON("$set" << BSON("count" << 1));
    auto finalDoc = encryptUpdateDocument(inputDoc, &keyVault);

    std::cout << finalDoc << std::endl;

    ASSERT_EQ(finalDoc["$set"]["count"].type(), BSONType::numberInt);
    ASSERT(finalDoc["$push"].eoo());
}

TEST_F(ServiceContextTest, FLE_Update_BadPush) {
    TestKeyVault keyVault;

    auto doc = BSON("value" << "123456");
    auto element = doc.firstElement();

    auto buf = generatePlaceholder(element, Operation::kInsert);
    auto inputDoc = BSON(
        "$push" << 123 << "$set"
                << BSON("encrypted" << BSONBinData(buf.data(), buf.size(), BinDataType::Encrypt)));
    ASSERT_THROWS_CODE(encryptUpdateDocument(inputDoc, &keyVault), DBException, 6371511);
}

TEST_F(ServiceContextTest, FLE_Update_PushToSafeContent) {
    TestKeyVault keyVault;

    auto doc = BSON("value" << "123456");
    auto element = doc.firstElement();

    auto buf = generatePlaceholder(element, Operation::kInsert);
    auto inputDoc = BSON(
        "$push" << 123 << "$set"
                << BSON("encrypted" << BSONBinData(buf.data(), buf.size(), BinDataType::Encrypt)));
    ASSERT_THROWS_CODE(encryptUpdateDocument(inputDoc, &keyVault), DBException, 6371511);
}

TEST_F(ServiceContextTest, FLE_Update_PushToOtherfield) {
    TestKeyVault keyVault;

    auto doc = BSON("value" << "123456");
    auto element = doc.firstElement();

    auto buf = generatePlaceholder(element, Operation::kInsert);
    auto inputDoc = BSON(
        "$push" << BSON("abc" << 123) << "$set"
                << BSON("encrypted" << BSONBinData(buf.data(), buf.size(), BinDataType::Encrypt)));
    auto finalDoc = encryptUpdateDocument(inputDoc, &keyVault);
    std::cout << finalDoc << std::endl;

    ASSERT_TRUE(finalDoc["$set"]["encrypted"].isBinData(BinDataType::Encrypt));
    ASSERT_TRUE(finalDoc["$push"]["abc"].type() == BSONType::numberInt);
    ASSERT_TRUE(finalDoc["$push"][kSafeContent]["$each"].type() == BSONType::array);
    ASSERT_EQ(finalDoc["$push"][kSafeContent]["$each"].Array().size(), 1);
    ASSERT_TRUE(
        finalDoc["$push"][kSafeContent]["$each"].Array()[0].isBinData(BinDataType::BinDataGeneral));
}

TEST_F(ServiceContextTest, FLE_Update_GetRemovedTags) {
    PrfBlock tag1 = decodePrf("BD53ACAC665EDD01E0CA30CB648B2B8F4967544047FD4E7D12B1A9BF07339928");
    PrfBlock tag2 = decodePrf("C17FDF249DE234F9AB15CD95137EA7EC82AE4E5B51F6BFB0FC1B8FEB6800F74C");
    ServerDerivedFromDataToken serverDerivedFromDataToken(
        decodePrf("986F23F132FF7F14F748AC69373CFC982AD0AD4BAD25BE92008B83AB43E96029"));
    ServerDataEncryptionLevel1Token serverToken(
        decodePrf("786F23F132FF7F14F748AC69373CFC982AD0AD4BAD25BE92008B83AB437EC82A"));

    std::vector<uint8_t> clientBlob(64);
    PrfBlock nullBlock;

    FLE2InsertUpdatePayloadV2 iup;
    iup.setServerEncryptionToken(serverToken);
    iup.setServerDerivedFromDataToken(serverDerivedFromDataToken);
    iup.setIndexKeyId(indexKeyId);
    iup.setValue(clientBlob);
    iup.setType(stdx::to_underlying(BSONType::string));
    iup.setContentionFactor(0);
    // these two tokens below are not used in fromUnencrypted
    iup.setEdcDerivedToken(EDCDerivedFromDataTokenAndContentionFactorToken(nullBlock));
    iup.setEscDerivedToken(ESCDerivedFromDataTokenAndContentionFactorToken(nullBlock));

    auto value1 = FLE2IndexedEqualityEncryptedValueV2::fromUnencrypted(iup, tag1, 1);
    auto value2 = FLE2IndexedEqualityEncryptedValueV2::fromUnencrypted(iup, tag2, 1);

    auto value1Blob = uassertStatusOK(value1.serialize());
    auto value2Blob = uassertStatusOK(value2.serialize());

    auto typeByte = static_cast<uint8_t>(EncryptedBinDataType::kFLE2EqualityIndexedValueV2);
    ASSERT_EQ(value1Blob[0], typeByte);
    ASSERT_EQ(value2Blob[0], typeByte);

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
                                         ConstDataRange(value1Blob));
    std::vector<EDCIndexedFields> v1Fields = {{v1ValueBlob, "a"}};
    ASSERT_THROWS_CODE(EDCServerCollection::getRemovedTags(v1Fields, empty), DBException, 7293204);

    // .. but not if the v1 field is also in the new document.
    tagsToPull = EDCServerCollection::getRemovedTags(v1Fields, v1Fields);
    ASSERT_EQ(tagsToPull.size(), 0);
}

TEST_F(ServiceContextTest, FLE_Update_GenerateUpdateToRemoveTags) {
    TestKeyVault keyVault;

    auto doc = BSON("value" << "123456");
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

    ASSERT_EQ(pullUpdate["$pull"].type(), BSONType::object);
    ASSERT_EQ(pullUpdate["$pull"][kSafeContent].type(), BSONType::object);
    ASSERT_EQ(pullUpdate["$pull"][kSafeContent]["$in"].type(), BSONType::array);
    auto tagsArray = pullUpdate["$pull"][kSafeContent]["$in"].Array();

    ASSERT_EQ(tagsArray.size(), removedTags.size());
    for (auto& tag : tagsArray) {
        ASSERT_TRUE(tag.isBinData(BinDataType::BinDataGeneral));
    }

    // Verify failure when list of tags is empty
    ASSERT_THROWS_CODE(EDCServerCollection::generateUpdateToRemoveTags({}), DBException, 7293203);
}

TEST_F(ServiceContextTest, CompactionHelpersTest_parseCompactionTokensTestEmpty) {
    const auto result = CompactionHelpers::parseCompactionTokens(BSONObj());
    ASSERT(result.empty());
}

TEST_F(ServiceContextTest, CompactionHelpersTest_parseCompactionTokensTest) {
    const ECOCToken token1(
        decodePrf("7076c7b05fb4be4fe585eed930b852a6d088a0c55f3c96b50069e8a26ebfb347"_sd));
    const ECOCToken token2(
        decodePrf("6ebfb347576b4be4fe585eed96d088a0c55f3c96b50069e8a230b852a05fb4be"_sd));
    const AnchorPaddingRootToken anchor2(
        decodePrf("7df988a08052e24dbe938c58b91ab00c812f58eabb3d4db1b047c3187d57f668"_sd));

    for (const bool includePaddingToken : {false, true}) {
        BSONObjBuilder builder;
        builder.appendBinData(
            "a.b.c", token1.toCDR().length(), BinDataType::BinDataGeneral, token1.toCDR().data());
        if (includePaddingToken) {
            BSONObjBuilder xy(builder.subobjStart("x.y"));
            xy.appendBinData("ecoc",
                             token2.toCDR().length(),
                             BinDataType::BinDataGeneral,
                             token2.toCDR().data());
            xy.appendBinData("anchorPaddingToken",
                             anchor2.toCDR().length(),
                             BinDataType::BinDataGeneral,
                             anchor2.toCDR().data());
            xy.doneFast();
        }

        const auto result = CompactionHelpers::parseCompactionTokens(builder.obj());
        ASSERT_EQ(result.size(), includePaddingToken ? 2UL : 1UL);

        ASSERT_EQ(result[0].fieldPathName, "a.b.c");
        ASSERT(result[0].token == token1);
        ASSERT(result[0].anchorPaddingToken == boost::none);

        if (includePaddingToken) {
            ASSERT_EQ(result[1].fieldPathName, "x.y");
            ASSERT(result[1].token == token2);
            ASSERT(result[1].anchorPaddingToken == anchor2);
        }
    }
}

TEST_F(ServiceContextTest, CompactionHelpersTest_parseCompactionTokensTestInvalidType) {
    ASSERT_THROWS_CODE(
        CompactionHelpers::parseCompactionTokens(BSON("foo" << "bar")), DBException, 6346801);
}

TEST_F(ServiceContextTest, CompactionHelpersTest_parseCompactionTokensTestInvalidSubType) {
    const std::array<std::uint8_t, 16> kUUID = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    BSONObjBuilder builder;
    builder.appendBinData("a.b.c", kUUID.size(), BinDataType::newUUID, kUUID.data());
    ASSERT_THROWS_CODE(
        CompactionHelpers::parseCompactionTokens(builder.obj()), DBException, 6346801);
}

TEST_F(ServiceContextTest, CompactionHelpersTest_parseCompactionTokensTestTooShort) {
    const auto kBadToken =
        hexblob::decode("7076c7b05fb4be4fe585eed930b852a6d088a0c55f3c96b50069e8a26ebfb3"_sd);
    constexpr auto kInvalidPrfLength = 9616300;

    BSONObjBuilder builder;
    builder.appendBinData("a.b.c", kBadToken.size(), BinDataType::BinDataGeneral, kBadToken.data());
    ASSERT_THROWS_CODE(
        CompactionHelpers::parseCompactionTokens(builder.obj()), DBException, kInvalidPrfLength);
}

TEST_F(ServiceContextTest, CompactionHelpersTest_parseCompactionTokensTestTooLong) {
    const auto kBadToken =
        hexblob::decode("7076c7b05fb4be4fe585eed930b852a6d088a0c55f3c96b50069e8a26ebfb34701"_sd);
    constexpr auto kInvalidPrfLength = 9616300;

    BSONObjBuilder builder;
    builder.appendBinData("a.b.c", kBadToken.size(), BinDataType::BinDataGeneral, kBadToken.data());
    ASSERT_THROWS_CODE(
        CompactionHelpers::parseCompactionTokens(builder.obj()), DBException, kInvalidPrfLength);
}

TEST_F(ServiceContextTest, CompactionHelpersTest_validateCompactionOrCleanupTokensTest) {
    EncryptedFieldConfig efc = getTestEncryptedFieldConfig();

    BSONObjBuilder builder;
    for (auto& field : efc.getFields()) {
        // validate fails until all fields are present
        ASSERT_THROWS_CODE(CompactionHelpers::validateCompactionOrCleanupTokens(
                               efc, builder.asTempObj(), "Compact"_sd),
                           DBException,
                           7294900);

        // validate doesn't care about the value, so this is fine
        builder.append(field.getPath(), "foo");
    }
    CompactionHelpers::validateCompactionOrCleanupTokens(efc, builder.asTempObj(), "Compact"_sd);

    // validate OK if obj has extra fields
    builder.append("abc.xyz", "foo");
    CompactionHelpers::validateCompactionOrCleanupTokens(efc, builder.obj(), "Compact"_sd);
}

TEST_F(ServiceContextTest, EDCServerCollectionTest_GenerateEDCTokens) {

    auto doc = BSON("sample" << 123456);
    auto element = doc.firstElement();

    auto value = ConstDataRange(element.value(), element.value() + element.valuesize());

    auto collectionToken = CollectionsLevel1Token::deriveFrom(getIndexKey());
    auto edcToken = EDCToken::deriveFrom(collectionToken);

    EDCDerivedFromDataToken edcDatakey = EDCDerivedFromDataToken::deriveFrom(edcToken, value);


    ASSERT_EQ(EDCServerCollection::generateEDCTokens(edcDatakey, 0).size(), 1);
    ASSERT_EQ(EDCServerCollection::generateEDCTokens(edcDatakey, 1).size(), 2);
    ASSERT_EQ(EDCServerCollection::generateEDCTokens(edcDatakey, 2).size(), 3);
    ASSERT_EQ(EDCServerCollection::generateEDCTokens(edcDatakey, 3).size(), 4);
}

TEST_F(ServiceContextTest, EDCServerCollectionTest_ValidateModifiedDocumentCompatibility) {
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


TEST_F(ServiceContextTest, EdgeCalcTest_SparsityConstraints) {
    ASSERT_THROWS_CODE(getEdgesInt32(1, 0, 8, 0, 0), AssertionException, 6775101);
    ASSERT_THROWS_CODE(getEdgesInt32(1, 0, 8, -1, 0), AssertionException, 6775101);
    ASSERT_THROWS_CODE(getEdgesInt64(1, 0, 8, 0, 0), AssertionException, 6775101);
    ASSERT_THROWS_CODE(getEdgesInt64(1, 0, 8, -1, 0), AssertionException, 6775101);
    ASSERT_THROWS_CODE(getEdgesDouble(1.0, 0.0, 8.0, 5, 0, 0), AssertionException, 6775101);
    ASSERT_THROWS_CODE(getEdgesDouble(1.0, 0.0, 8.0, 5, -1, 0), AssertionException, 6775101);
}

TEST_F(ServiceContextTest, EdgeCalcTest_TrimFactorConstraints) {
    ASSERT_THROWS_CODE(getEdgesInt32(1, 0, 7, 1, -1), AssertionException, 8574105);
    ASSERT_THROWS_CODE(getEdgesInt32(1, 0, 7, 1, 3), AssertionException, 8574105);
    ASSERT_THROWS_CODE(getEdgesInt64(1, 0, 7, 1, -1), AssertionException, 8574105);
    ASSERT_THROWS_CODE(getEdgesInt64(1, 0, 7, 1, 3), AssertionException, 8574105);
    ASSERT_THROWS_CODE(getEdgesDouble(1.0, 0.0, 5.0, 1, 1, -1), AssertionException, 8574105);
    ASSERT_THROWS_CODE(getEdgesDouble(1.0, 0.0, 5.0, 1, 1, 6), AssertionException, 8574105);
}

void doEdgeCalcTestIdentifyLeaf(std::unique_ptr<Edges> edges, StringData expectLeaf) {
    ASSERT_EQ(edges->getLeaf(), expectLeaf);
    auto edgeSet = edges->get();
    // sanity check edge set size vs edges->size()
    ASSERT_EQ(edgeSet.size(), edges->size());
    ASSERT_EQ(std::count_if(edgeSet.cbegin(),
                            edgeSet.cend(),
                            [expectLeaf](const auto& leaf) { return leaf == expectLeaf; }),
              1);
}

TEST_F(ServiceContextTest, EdgeCalcTest_IdentifyLeaf) {
    constexpr auto k42Leaf64 =
        "1000000000000000000000000000000000000000000000000000000000101010"_sd;
    doEdgeCalcTestIdentifyLeaf(getEdgesInt64(42, {}, {}, 1, 0), k42Leaf64);
    doEdgeCalcTestIdentifyLeaf(getEdgesInt64(42, {}, {}, 7, 0), k42Leaf64);
    doEdgeCalcTestIdentifyLeaf(getEdgesInt64(42, {}, {}, 1, 10), k42Leaf64);
    doEdgeCalcTestIdentifyLeaf(getEdgesInt64(42, {}, {}, 7, 10), k42Leaf64);
    doEdgeCalcTestIdentifyLeaf(getEdgesInt64(42, {}, {}, 15, 10), k42Leaf64);
    constexpr auto k42Leaf32 = "10000000000000000000000000101010"_sd;
    doEdgeCalcTestIdentifyLeaf(getEdgesInt32(42, {}, {}, 2, 0), k42Leaf32);
    doEdgeCalcTestIdentifyLeaf(getEdgesInt32(42, {}, {}, 11, 0), k42Leaf32);
    doEdgeCalcTestIdentifyLeaf(getEdgesInt32(42, {}, {}, 2, 9), k42Leaf32);
    doEdgeCalcTestIdentifyLeaf(getEdgesInt32(42, {}, {}, 11, 9), k42Leaf32);
    constexpr auto k42LeafDouble =
        "1100000001000101000000000000000000000000000000000000000000000000"_sd;
    doEdgeCalcTestIdentifyLeaf(getEdgesDouble(42, {}, {}, {}, 3, 0), k42LeafDouble);
    doEdgeCalcTestIdentifyLeaf(getEdgesDouble(42, {}, {}, {}, 13, 0), k42LeafDouble);
    doEdgeCalcTestIdentifyLeaf(getEdgesDouble(42, {}, {}, {}, 3, 8), k42LeafDouble);
    doEdgeCalcTestIdentifyLeaf(getEdgesDouble(42, {}, {}, {}, 13, 8), k42LeafDouble);
    constexpr auto k42LeafDecimal128 =
        "10101110001110011011100011110011101110001010011010111110001110010011001110111000101010110010100111111111111111111110100000000000"_sd;
    doEdgeCalcTestIdentifyLeaf(getEdgesDecimal128(Decimal128(42), {}, {}, {}, 5, 0),
                               k42LeafDecimal128);
    doEdgeCalcTestIdentifyLeaf(getEdgesDecimal128(Decimal128(42), {}, {}, {}, 17, 0),
                               k42LeafDecimal128);
    doEdgeCalcTestIdentifyLeaf(getEdgesDecimal128(Decimal128(42), {}, {}, {}, 5, 7),
                               k42LeafDecimal128);
    doEdgeCalcTestIdentifyLeaf(getEdgesDecimal128(Decimal128(42), {}, {}, {}, 17, 7),
                               k42LeafDecimal128);
}

TEST_F(ServiceContextTest, MinCoverCalcTest_MinCoverConstraints) {
    ASSERT(minCoverInt32(2, true, 1, true, 0, 7, 1, 0).empty());
    ASSERT(minCoverInt64(2, true, 1, true, 0, 7, 1, 0).empty());
    ASSERT(minCoverDouble(2, true, 1, true, 0, 7, boost::none, 1, 0).empty());
    ASSERT(minCoverDecimal128(
               Decimal128(2), true, Decimal128(1), true, Decimal128(0), Decimal128(7), 5, 1, 0)
               .empty());
}

TEST_F(ServiceContextTest, EdgeCalcTest_SubstringTagCalculators) {
    // Expected values were calculated from OST paper's msize formula from the SubTree function.
    ASSERT_EQ(0, msizeForSubstring(10, 48, 100, 200));         // (padlen=11) < lb
    ASSERT_EQ(1, msizeForSubstring(30, 43, 100, 200));         // lb == (padlen=43) < ub
    ASSERT_EQ(595, msizeForSubstring(30, 10, 100, 200));       // lb < (padlen=43) < ub
    ASSERT_EQ(9316, msizeForSubstring(150, 20, 155, 200));     // ub == (padlen=155) < mlen
    ASSERT_EQ(9301, msizeForSubstring(150, 20, 150, 200));     // ub < (padlen=155) < mlen
    ASSERT_EQ(87815, msizeForSubstring(1004, 10, 100, 1019));  // ub < mlen == (padlen=1019)
    ASSERT_EQ(87815, msizeForSubstring(1030, 10, 100, 1019));  // ub < mlen < (padlen=1043)
    ASSERT_THROWS_CODE(
        msizeForSubstring(INT32_MAX, 1, INT32_MAX, INT32_MAX), DBException, 10384600);
    ASSERT_THROWS_CODE(
        msizeForSubstring(INT32_MAX - 128, 1, INT32_MAX, INT32_MAX), DBException, 10384601);

    ASSERT_EQ(87815, maxTagsForSubstring(10, 100, 1019));    // lb < ub < mlen
    ASSERT_EQ(510555, maxTagsForSubstring(10, 1019, 1019));  // lb < ub == mlen
    ASSERT_EQ(1003, maxTagsForSubstring(17, 17, 1019));      // lb == ub < mlen
    ASSERT_EQ(1, maxTagsForSubstring(1019, 1019, 1019));     // lb == ub == mlen
}

TEST_F(ServiceContextTest, EdgeCalcTest_SuffixPrefixTagCalculators) {
    // Expected values were calculated from OST paper's msize formulas from the SuffTree and
    // PrefTree functions.
    ASSERT_EQ(0, msizeForSuffixOrPrefix(10, 48, 100));     // (padlen=11) < lb
    ASSERT_EQ(1, msizeForSuffixOrPrefix(30, 43, 100));     // lb == (padlen=43) < ub
    ASSERT_EQ(34, msizeForSuffixOrPrefix(30, 10, 100));    // lb < (padlen=43) < ub
    ASSERT_EQ(136, msizeForSuffixOrPrefix(150, 20, 155));  // ub == (padlen=155)
    ASSERT_EQ(131, msizeForSuffixOrPrefix(150, 20, 150));  // ub < (padlen=155)
    ASSERT_THROWS_CODE(msizeForSuffixOrPrefix(INT32_MAX, 1, INT32_MAX), DBException, 10384600);

    ASSERT_EQ(91, maxTagsForSuffixOrPrefix(10, 100));  // lb < ub
    ASSERT_EQ(1, maxTagsForSuffixOrPrefix(100, 100));  // lb == ub
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
                          int trimFactor,
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
    edgesInfo.setTrimFactor(trimFactor);

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
                                   int trimFactor,
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
    edgesInfo.setTrimFactor(trimFactor);

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

TEST_F(ServiceContextTest, MinCoverInterfaceTest_Int32_Basic) {
    assertMinCoverResult(7, true, 32, true, 0, 32, 1, 0, {"000111", "001", "01", "100000"});
    assertMinCoverResult(7, false, 32, false, 0, 32, 1, 0, {"001", "01"});
    assertMinCoverResult(7, true, 32, false, 0, 32, 1, 0, {"000111", "001", "01"});
    assertMinCoverResult(7, true, 32, false, 0, 32, 1, 0, {"000111", "001", "01"});

    assertMinCoverResult(7, true, 32, true, 0, 32, 1, 3, {"000111", "001", "010", "011", "100000"});
    assertMinCoverResult(
        7, false, 32, false, 0, 32, 1, 4, {"0010", "0011", "0100", "0101", "0110", "0111"});
    assertMinCoverResult(7, true, 32, false, 0, 32, 1, 2, {"000111", "001", "01"});
    assertMinCoverResult(7,
                         true,
                         32,
                         false,
                         0,
                         32,
                         1,
                         5,
                         {"000111",
                          "00100",
                          "00101",
                          "00110",
                          "00111",
                          "01000",
                          "01001",
                          "01010",
                          "01011",
                          "01100",
                          "01101",
                          "01110",
                          "01111"});
}

TEST_F(ServiceContextTest, MinCoverInterfaceTest_Int64_Basic) {
    assertMinCoverResult(0LL,
                         true,
                         823LL,
                         true,
                         -1000000000000000LL,
                         8070450532247928832LL,
                         2,
                         0,
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
                         0,
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
                         0,
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
                         0,
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

    assertMinCoverResult(0LL,
                         true,
                         823LL,
                         false,
                         -1000000000000000LL,
                         8070450532247928832LL,
                         2,
                         54,
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
                         true,
                         823LL,
                         false,
                         -1000000000000000LL,
                         8070450532247928832LL,
                         2,
                         55,
                         {
                             "00000000000001110001101011111101010010011000110100000000",
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
}

TEST_F(ServiceContextTest, MinCoverInterfaceTest_Double_Basic) {
    assertMinCoverResult(23.5,
                         true,
                         35.25,
                         true,
                         0.0,
                         1000.0,
                         1,
                         0,
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
                         0,
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
                         0,
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
                         0,
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
    assertMinCoverResult(23.5,
                         true,
                         35.25,
                         false,
                         0.0,
                         1000.0,
                         1,
                         13,
                         {
                             "11000000001101111",
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
                         14,
                         {
                             "11000000001101111",
                             "11000000001110",
                             "11000000001111",
                             "1100000001000000",
                             "11000000010000010",
                             "1100000001000001100",
                         });
}

TEST_F(ServiceContextTest, MinCoverInterfaceTest_Decimal_Basic) {
    assertMinCoverResult(
        Decimal128(23.5),
        true,
        Decimal128(35.25),
        true,
        Decimal128(0.0),
        Decimal128(1000.0),
        1,
        0,
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
        0,
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
        0,
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
        0,
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
    assertMinCoverResult(
        Decimal128(23.5),
        true,
        Decimal128(35.25),
        true,
        Decimal128(0.0),
        Decimal128(1000.0),
        1,
        19,
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
        true,
        Decimal128(35.25),
        true,
        Decimal128(0.0),
        Decimal128(1000.0),
        1,
        20,
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
            "10101110001110010110",
            "10101110001110010111",
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

TEST_F(ServiceContextTest, MinCoverInterfaceTest_InfiniteRangeBounds) {
    assertMinCoverResult(7.0,
                         true,
                         std::numeric_limits<double>::infinity(),
                         true,
                         0.0,
                         32.0,
                         1,
                         0,
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
                         0,
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
                         0,
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
                         3,
                         {
                             "100",
                             "101",
                             "11000000000",
                             "1100000000100000000000000000000000000000000000000000000000000000",
                         });
}

TEST_F(ServiceContextTest, MinCoverInteraceTest_InvalidBounds) {
    assertMinCoverResult(7, true, 7, false, 0, 32, 1, 0, {});
    assertMinCoverResult(7LL, true, 7LL, false, 0LL, 32LL, 1, 0, {});
    assertMinCoverResult(7.0, true, 7.0, false, 0.0, 32.0, 1, 0, {});

    assertMinCoverResult(7, false, 7, true, 0, 32, 1, 0, {});
    assertMinCoverResult(7LL, false, 7LL, true, 0LL, 32LL, 1, 0, {});
    assertMinCoverResult(7.0, false, 7.0, true, 0.0, 32.0, 1, 0, {});

    ASSERT_THROWS_CODE(
        assertMinCoverResult(1, false, 1, false, 0, 1, 1, 0, {}), AssertionException, 6901316);
    ASSERT_THROWS_CODE(
        assertMinCoverResult(0, true, 0, false, 0, 7, 1, 0, {}), AssertionException, 6901317);


    ASSERT_THROWS(assertMinCoverResult(1, true, 2, true, 0, 7, 1, -1, {}), AssertionException);
    ASSERT_THROWS_CODE(
        assertMinCoverResult(1, true, 2, true, 0, 7, 1, 3, {}), AssertionException, 8574106);
}

// Test point queries and that trimming bitstrings is correct in precision mode
TEST_F(ServiceContextTest, MinCoverInteraceTest_Precision_Equal) {
    assertMinCoverResultPrecision(
        3.14159, true, 3.14159, true, 0.0, 10.0, 1, 2, 0, {"00100111010"});
    assertMinCoverResultPrecision(Decimal128(3.14159),
                                  true,
                                  Decimal128(3.14159),
                                  true,
                                  Decimal128(0.0),
                                  Decimal128(10.0),
                                  1,
                                  2,
                                  0,
                                  {"00100111010"});

    assertMinCoverResultPrecision(3.1, true, 3.1, true, 0.0, 12.0, 1, 1, 0, {"00011111"});
    assertMinCoverResultPrecision(Decimal128(3.1),
                                  true,
                                  Decimal128(3.1),
                                  true,
                                  Decimal128(0.0),
                                  Decimal128(12.0),
                                  1,
                                  1,
                                  0,
                                  {"00011111"});
    assertMinCoverResultPrecision(3.1, true, 3.1, true, 0.0, 12.0, 1, 1, 7, {"00011111"});
    assertMinCoverResultPrecision(Decimal128(3.1),
                                  true,
                                  Decimal128(3.1),
                                  true,
                                  Decimal128(0.0),
                                  Decimal128(12.0),
                                  1,
                                  1,
                                  7,
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

class EdgeTestFixture : public unittest::Test {
public:
    static constexpr int kMaxPrecisionDouble = 15;
    static constexpr int kMaxPrecisionDecimal128 = 34;

    template <typename T>
    static QueryTypeConfig makeRangeQueryTypeConfig(T lb,
                                                    T ub,
                                                    const boost::optional<uint32_t>& precision,
                                                    int sparsity) {
        QueryTypeConfig config;
        config.setQueryType(QueryTypeEnum::Range);
        if constexpr (std::is_same_v<T, long>) {
            // Type aliasing gets a little weird. int64_t -> long, but Value(long) = delete, and
            // int64_t ~= long long anyway. Ignore the distinction for the purposes of this test.
            config.setMin(Value(static_cast<long long>(lb)));
            config.setMax(Value(static_cast<long long>(ub)));
        } else {
            config.setMin(Value(lb));
            config.setMax(Value(ub));
        }
        config.setSparsity(sparsity);
        if (precision) {
            config.setPrecision(*precision);
        }
        config.setTrimFactor(0);
        return config;
    }

    template <typename T, typename GetEdges>
    static void assertEdgesLengthMatch(const T& lb,
                                       const T& ub,
                                       const boost::optional<std::uint32_t>& precision,
                                       int sparsity,
                                       GetEdges getEdges,
                                       BSONType fieldType) {
        const auto edges = [&] {
            if constexpr (std::is_same_v<T, double> || std::is_same_v<T, Decimal128>) {
                return getEdges(lb, lb, ub, precision, sparsity, 0);
            } else if constexpr (std::is_same_v<T, Date_t>) {
                auto lbInMillis = lb.toMillisSinceEpoch();
                auto ubInMillis = ub.toMillisSinceEpoch();
                return getEdges(lbInMillis, lbInMillis, ubInMillis, sparsity, 0);
            } else {
                return getEdges(lb, lb, ub, sparsity, 0);
            }
        }();
        const auto expect = edges->get().size();
        // The actual size of edges should be equal to edges->size(). This is a sanity check.
        ASSERT_EQ(expect, edges->size());
        const auto calculated = getEdgesLength(
            fieldType, "rangeField"_sd, makeRangeQueryTypeConfig(lb, ub, precision, sparsity));
        if (expect != calculated) {
            // Context for the exception we're about to throw.
            LOGV2(8574790,
                  "Mismatched edge length prediction",
                  "lb"_attr = lb,
                  "ub"_attr = ub,
                  "sparsity"_attr = sparsity,
                  "precision"_attr = precision.get_value_or(-1),
                  "leafSize"_attr = edges->getLeaf().size(),
                  "expect"_attr = expect,
                  "calculated"_attr = calculated);
        }
        ASSERT_EQ(expect, calculated);
    }

    template <typename T, typename GetEdges>
    static void runEdgesLengthTestForFunamentalType(GetEdges getEdges, BSONType fieldType) {
        constexpr auto low = std::numeric_limits<T>::lowest();
        constexpr auto max = std::numeric_limits<T>::max();

        std::vector<T> testVals{low,
                                low / 2,
                                -1000000,
                                -65537,
                                -1000,
                                -10,
                                -1,
                                0,
                                1,
                                10,
                                1000,
                                65537,
                                1000000,
                                max / 2,
                                max};
        std::vector<boost::optional<uint32_t>> testPrecisions = {boost::none};
        if constexpr (std::is_same_v<T, double>) {
            testVals.push_back(1.1);
            testVals.push_back(-1.1);
            testVals.push_back(2.71828182);
            testVals.push_back(3.14159265);

            testPrecisions.clear();
            for (int i = 1; i <= kMaxPrecisionDouble; ++i) {
                testPrecisions.push_back(i);
            }
        }

        for (int sparsity = 1; sparsity <= 8; ++sparsity) {
            for (const T lb : testVals) {
                for (const T ub : testVals) {
                    if (lb >= ub) {
                        // getEdgesT has a check for min < max, tested elsewhere.
                        continue;
                    }
                    for (const auto& precision : testPrecisions) {
                        boost::optional<ErrorCodes::Error> thrownCode;
                        if constexpr (std::is_same_v<T, double>) {
                            // getEdgesLength will throw if the precision is invalid for the
                            // min & max.
                            // Here we do a pre-check to see if that validation will fail on those
                            // assertions, and if so, also assert that the same error code is thrown
                            // by the test.
                            try {
                                auto qtc = makeRangeQueryTypeConfig(lb, ub, precision, sparsity);
                                validateRangeIndex(BSONType::numberDouble, "rangeField"_sd, qtc);
                            } catch (DBException& e) {
                                if (e.code() == 6966805 || e.code() == 6966806 ||
                                    e.code() == 9157100 || e.code() == 9178801 ||
                                    e.code() == 9178802 || e.code() == 9178803 ||
                                    e.code() == 9178804 || e.code() == 9178805) {
                                    thrownCode = e.code();
                                } else {
                                    throw;
                                }
                            }
                        }
                        try {
                            assertEdgesLengthMatch(
                                lb, ub, precision, sparsity, getEdges, fieldType);
                        } catch (DBException& e) {
                            if (thrownCode) {
                                if constexpr (!std::is_same_v<T, double>) {
                                    ASSERT_EQ(e.code(), *thrownCode);
                                }
                            } else {
                                throw;
                            }
                        }
                    }
                }
            }
        }
    }
};

TEST_F(EdgeTestFixture, getEdgesLengthInt32) {
    runEdgesLengthTestForFunamentalType<int32_t>(getEdgesInt32, BSONType::numberInt);
}

TEST_F(EdgeTestFixture, getEdgesLengthInt64) {
    runEdgesLengthTestForFunamentalType<int64_t>(getEdgesInt64, BSONType::numberLong);
}

TEST_F(EdgeTestFixture, getEdgesLengthDouble) {
    runEdgesLengthTestForFunamentalType<double>(getEdgesDouble, BSONType::numberDouble);
}

// Decimal128 is less well templated than the fundamental types,
// Check a smaller, but still representative sample of values.
// Additionally, when Decimal128 is used in EncryptionInformation
TEST_F(EdgeTestFixture, getEdgesLengthDecimal128) {
    const std::vector<Decimal128> testVals{
        Decimal128(-1000000),
        Decimal128(-1000),
        Decimal128(-10),
        Decimal128::kNormalizedZero,
        Decimal128(10),
        Decimal128(1000),
        Decimal128(1000000),
    };

    for (int sparsity = 1; sparsity <= 8; ++sparsity) {
        for (const auto& lb : testVals) {
            for (const auto& ub : testVals) {
                if (lb >= ub) {
                    continue;
                }
                for (std::uint32_t precision = 1; precision <= kMaxPrecisionDecimal128;
                     ++precision) {
                    // getEdgesLength may throw if the min,max,precision combination can't be
                    // used for the precision-mode encoding.
                    try {
                        if (canUsePrecisionMode(lb, ub, precision, nullptr)) {
                            assertEdgesLengthMatch(lb,
                                                   ub,
                                                   precision,
                                                   sparsity,
                                                   getEdgesDecimal128,
                                                   BSONType::numberDecimal);
                        } else {
                            ASSERT_THROWS_CODE(assertEdgesLengthMatch(lb,
                                                                      ub,
                                                                      precision,
                                                                      sparsity,
                                                                      getEdgesDecimal128,
                                                                      BSONType::numberDecimal),
                                               DBException,
                                               9157101);
                        }
                    } catch (DBException& e) {
                        if (!(e.code() == 9178808 || e.code() == 9178809 || e.code() == 9178810 ||
                              e.code() == 9178811 || e.code() == 9178812)) {
                            throw;
                        }
                    }
                }
            }
        }
    }
}

#define ASSERT_EIBB_OVERFLOW(v, ub, lb, prc, z)                                                \
    {                                                                                          \
        auto _ost = getTypeInfoDecimal128(Decimal128(v), Decimal128(lb), Decimal128(ub), prc); \
        ASSERT_EQ(_ost.max.str(), "340282366920938463463374607431768211455");                  \
        ASSERT_EQ(_ost.value, z);                                                              \
    }

constexpr double INT_64_MAX_DOUBLE = static_cast<double>(std::numeric_limits<uint64_t>::max());

TEST_F(EdgeTestFixture, canUsePrecisionMode) {
#define CAN_USE_PRECISION_MODE(lb, ub, prc, expected, expected_bits_out) \
    {                                                                    \
        uint32_t bits_out = 0;                                           \
        auto result = canUsePrecisionMode(lb, ub, prc, &bits_out);       \
        ASSERT_EQ(result, expected);                                     \
        ASSERT_EQ(expected_bits_out, bits_out);                          \
    }

#define CAN_USE_PRECISION_MODE_ERRORS(lb, ub, prc, code)                                  \
    {                                                                                     \
        ASSERT_THROWS_CODE(canUsePrecisionMode(lb, ub, prc, nullptr), DBException, code); \
    }

    /**
     * Test Cases: (min, max, precision) -> (bool, bits_out)
     *
     * (1, 16, 0) -> true, 4
     * (0, 16, 0) -> true, 5
     * (DOUBLE_MAX, DOUBLE_MIN, 0) -> false, 1024?
     * (1, 2^53, 0) -> true, 53
     * (0, 2^53, 0) -> true, 54
     */

    CAN_USE_PRECISION_MODE(1, 16, 0, true, 4);
    CAN_USE_PRECISION_MODE(0, 16, 0, true, 5);
    // 2^53 + 1 is where double starts to lose precision, so we need to ensure that we get the
    // correct value for max_bits out.
    CAN_USE_PRECISION_MODE(1, 9007199254740992, 0, true, 53);
    CAN_USE_PRECISION_MODE(0, 9007199254740992, 0, true, 54);

    CAN_USE_PRECISION_MODE(2.718281, 314.159265, 6, true, 29);

    // precision too large
    CAN_USE_PRECISION_MODE_ERRORS(
        Decimal128("1"), Decimal128("2"), std::numeric_limits<uint32_t>::max(), 9125501);
    CAN_USE_PRECISION_MODE_ERRORS(1, 2, std::numeric_limits<uint32_t>::max(), 9125503);
    // 10^prc results in infinity above 308 for doubles, since the largest double value is
    // 1.7976931348623157x10^308
    CAN_USE_PRECISION_MODE_ERRORS(0, 1, 309, 9125504);
    // 10^prc results in infinity above 6144 for decimal, since the largest decimal128 value
    // is 9.99999...x10^6144
    CAN_USE_PRECISION_MODE_ERRORS(Decimal128("0"), Decimal128("1"), 6145, 9125502);

    CAN_USE_PRECISION_MODE_ERRORS(2.710000, 314.150000, 2, 9178801);
    CAN_USE_PRECISION_MODE_ERRORS(314.150000, 350, 2, 9178802);

    CAN_USE_PRECISION_MODE_ERRORS(
        static_cast<double>(9007199254740992), INT_64_MAX_DOUBLE, 0, 9178803);
    CAN_USE_PRECISION_MODE_ERRORS(
        -1 * INT_64_MAX_DOUBLE, -1 * static_cast<double>(9007199254740992), 0, 9178804);
    CAN_USE_PRECISION_MODE_ERRORS(-92233720368547, 92233720368547, 5, 9178805);

    CAN_USE_PRECISION_MODE(Decimal128("1"), Decimal128("16"), 0, true, 4);
    CAN_USE_PRECISION_MODE(Decimal128("0"), Decimal128("16"), 0, true, 5);

    // CAN_USE_PRECISION_MODE(Decimal128("1"), Decimal128::kLargestPositive, 0, false, 0);
    // It is unclear where Decimal128 looses precision, so we choose an arbitrarily large value
    // and make sure that max_bits is correct for that boundary.
    CAN_USE_PRECISION_MODE(
        Decimal128("1"), Decimal128("324518553658426726783156020576256"), 0, true, 108);
    CAN_USE_PRECISION_MODE(
        Decimal128("0"), Decimal128("324518553658426726783156020576256"), 0, true, 109);

    CAN_USE_PRECISION_MODE(Decimal128("-100000000000000000000000000000000"),
                           Decimal128("170141183460469231731687303715880000000"),
                           0,
                           false,
                           0);

    CAN_USE_PRECISION_MODE_ERRORS(
        Decimal128("788545.12392843"), Decimal128("4607431769000000.129834923"), 4, 9178808);
    CAN_USE_PRECISION_MODE_ERRORS(
        Decimal128("788545.12392843"), Decimal128("7885451.2"), 4, 9178809);
    CAN_USE_PRECISION_MODE_ERRORS(Decimal128("324518553658426726783156020576256"),
                                  Decimal128("340282366920938463463374607431768211455"),
                                  10,
                                  9178810);

    CAN_USE_PRECISION_MODE_ERRORS(Decimal128("-340282366920938463463374607431768211455"),
                                  Decimal128("-3245185536584267267831560"),
                                  10,
                                  9178811);

    CAN_USE_PRECISION_MODE_ERRORS(Decimal128("-17014118346046923173168730371588.0000000"),
                                  Decimal128("17014118346046923173168730371588.0000000"),
                                  7,
                                  9178812);

    CAN_USE_PRECISION_MODE_ERRORS(Decimal128("788545.000000"),
                                  Decimal128("340282366920938463463374607431769000000.000000"),
                                  0,
                                  9178810);

#undef CAN_USE_PRECISION_MODE
#undef CAN_USE_PRECISION_MODE_ERRORS
}

TEST_F(EdgeTestFixture, getEdgesLengthDate) {
    const std::vector<Date_t> testVals{
        Date_t::min(),
        Date_t::now() - Days{7},
        Date_t::now(),
        Date_t::now() + Days{7},
        Date_t::max(),
    };

    for (int sparsity = 1; sparsity <= 8; ++sparsity) {
        for (const auto& lb : testVals) {
            for (const auto& ub : testVals) {
                if (lb >= ub) {
                    continue;
                }
                assertEdgesLengthMatch(
                    lb, ub, boost::none, sparsity, getEdgesInt64, BSONType::date);
            }
        }
    }
}

class AnchorPaddingFixture : public ServiceContextTest {
public:
    static constexpr auto kAnchorPaddingRootHex =
        "4312890F621FE3CA7497C3405DFD8AAF46A578C77F7404D28C12BA853A4D3327"_sd;

    const AnchorPaddingRootToken _rootToken{decodePrf(kAnchorPaddingRootHex)};
    const AnchorPaddingKeyToken _keyToken = AnchorPaddingKeyToken::deriveFrom(_rootToken);
    const AnchorPaddingValueToken _valueToken = AnchorPaddingValueToken::deriveFrom(_rootToken);
};

TEST_F(AnchorPaddingFixture, generatePaddingDocument) {
    constexpr std::uint64_t kId = 42;
    auto doc =
        ESCCollectionAnchorPadding::generatePaddingDocument(&hmacCtx, _keyToken, _valueToken, kId);
    ASSERT_EQ(doc.nFields(), 2UL);

    // _id := F_k(bot || id)
    {
        // kHashOf042 = SHA256_HMAC(Key, 0 || 42), numbers as 64bit LE integers
        constexpr auto kHashOf042 =
            "0564ba5c84f27f20dd5a0ed69cace035983c50ccb4fa94244a475ab7c1e891ee"_sd;
        auto expectId = decodePrf(kHashOf042);

        auto idElem = doc["_id"_sd];
        ASSERT_EQ(idElem.type(), BSONType::binData);
        ASSERT_EQ(idElem.binDataType(), BinDataGeneral);

        int actualIdLen = 0;
        const char* actualIdPtr = idElem.binData(actualIdLen);
        ASSERT_EQ(actualIdLen, expectId.size());

        PrfBlock actualId = blockToArray(StringData(actualIdPtr, actualIdLen));
        ASSERT(expectId == actualId);
    }

    // value := Enc(0 || 0)
    {
        auto valueElem = doc["value"_sd];
        ASSERT_EQ(valueElem.type(), BSONType::binData);
        ASSERT_EQ(valueElem.binDataType(), BinDataGeneral);
        int len = 0;
        const char* value = valueElem.binData(len);
        auto swDecrypt = FLEUtil::decryptData(_valueToken.toCDR(), ConstDataRange(value, len));
        ASSERT_OK(swDecrypt.getStatus());
        auto dec = std::move(swDecrypt.getValue());
        ASSERT_TRUE(std::all_of(dec.begin(), dec.end(), [](auto b) { return b == 0; }));
    }
}

TEST_F(ServiceContextTest, fleEncryptAndDecrypt) {
    PrfBlock key;
    uassertStatusOK(crypto::engineRandBytes(DataRange(key)));
    std::vector<uint8_t> plaintext{'a', 'b', 'c', 'd', 'e', 'f'};
    auto encrypt1 = uassertStatusOK(FLEUtil::encryptData(key, plaintext));
    auto encrypt2 = uassertStatusOK(FLEUtil::encryptData(key, plaintext));
    // Ensure that we are correctly generating random IVs in encryptData.
    ASSERT_NE(encrypt1, encrypt2);
    auto decrypt1 = uassertStatusOK(FLEUtil::decryptData(key, encrypt1));
    auto decrypt2 = uassertStatusOK(FLEUtil::decryptData(key, encrypt2));
    ASSERT_EQ(decrypt1, decrypt2);
    ASSERT_EQ(decrypt1, plaintext);
}

}  // namespace mongo
