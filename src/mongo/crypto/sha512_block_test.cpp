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

#include "mongo/crypto/sha512_block.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"

#include <ostream>
#include <string>

namespace mongo {
namespace {

ConstDataRange makeTestItem(StringData sd) {
    return ConstDataRange(sd.data(), sd.size());
}

// SHA-512 test vectors from http://csrc.nist.gov/groups/ST/toolkit/documents/Examples/SHA_All.pdf
const struct {
    std::initializer_list<ConstDataRange> msg;
    SHA512Block hash;
} sha512Tests[] = {
    {{makeTestItem("abc")},
     SHA512Block::HashType{0xdd, 0xaf, 0x35, 0xa1, 0x93, 0x61, 0x7a, 0xba, 0xcc, 0x41, 0x73,
                           0x49, 0xae, 0x20, 0x41, 0x31, 0x12, 0xe6, 0xfa, 0x4e, 0x89, 0xa9,
                           0x7e, 0xa2, 0x0a, 0x9e, 0xee, 0xe6, 0x4b, 0x55, 0xd3, 0x9a, 0x21,
                           0x92, 0x99, 0x2a, 0x27, 0x4f, 0xc1, 0xa8, 0x36, 0xba, 0x3c, 0x23,
                           0xa3, 0xfe, 0xeb, 0xbd, 0x45, 0x4d, 0x44, 0x23, 0x64, 0x3c, 0xe8,
                           0x0e, 0x2a, 0x9a, 0xc9, 0x4f, 0xa5, 0x4c, 0xa4, 0x9f}},

    {{makeTestItem("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjkl"
                   "mnopqklmnopqrlmnopqrsmnopqrstnopqrstu")},
     SHA512Block::HashType{0x8e, 0x95, 0x9b, 0x75, 0xda, 0xe3, 0x13, 0xda, 0x8c, 0xf4, 0xf7,
                           0x28, 0x14, 0xfc, 0x14, 0x3f, 0x8f, 0x77, 0x79, 0xc6, 0xeb, 0x9f,
                           0x7f, 0xa1, 0x72, 0x99, 0xae, 0xad, 0xb6, 0x88, 0x90, 0x18, 0x50,
                           0x1d, 0x28, 0x9e, 0x49, 0x00, 0xf7, 0xe4, 0x33, 0x1b, 0x99, 0xde,
                           0xc4, 0xb5, 0x43, 0x3a, 0xc7, 0xd3, 0x29, 0xee, 0xb6, 0xdd, 0x26,
                           0x54, 0x5e, 0x96, 0xe5, 0x5b, 0x87, 0x4b, 0xe9, 0x09}},

    {{makeTestItem("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopj"),
      makeTestItem("klmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")},
     SHA512Block::HashType{0x8e, 0x95, 0x9b, 0x75, 0xda, 0xe3, 0x13, 0xda, 0x8c, 0xf4, 0xf7,
                           0x28, 0x14, 0xfc, 0x14, 0x3f, 0x8f, 0x77, 0x79, 0xc6, 0xeb, 0x9f,
                           0x7f, 0xa1, 0x72, 0x99, 0xae, 0xad, 0xb6, 0x88, 0x90, 0x18, 0x50,
                           0x1d, 0x28, 0x9e, 0x49, 0x00, 0xf7, 0xe4, 0x33, 0x1b, 0x99, 0xde,
                           0xc4, 0xb5, 0x43, 0x3a, 0xc7, 0xd3, 0x29, 0xee, 0xb6, 0xdd, 0x26,
                           0x54, 0x5e, 0x96, 0xe5, 0x5b, 0x87, 0x4b, 0xe9, 0x09}},

};

TEST(CryptoVectors, SHA512) {
    size_t numTests = sizeof(sha512Tests) / sizeof(sha512Tests[0]);
    for (size_t i = 0; i < numTests; i++) {
        SHA512Block result = SHA512Block::computeHash(sha512Tests[i].msg);
        ASSERT(sha512Tests[i].hash == result) << "Failed SHA512 iteration " << i;
    }
}

const int maxKeySize = 80;
const int maxDataSize = 54;
// HMAC-SHA-512 test vectors from https://tools.ietf.org/html/rfc4231#section-4.2
const struct {
    unsigned char key[maxKeySize];
    int keyLen;
    unsigned char data[maxDataSize];
    int dataLen;
    SHA512Block hash;
} hmacSha512Tests[] = {
    // RFC test case 1
    {{0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
      0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b},
     20,
     {0x48, 0x69, 0x20, 0x54, 0x68, 0x65, 0x72, 0x65},
     8,
     SHA512Block::HashType{0x87, 0xaa, 0x7c, 0xde, 0xa5, 0xef, 0x61, 0x9d, 0x4f, 0xf0, 0xb4,
                           0x24, 0x1a, 0x1d, 0x6c, 0xb0, 0x23, 0x79, 0xf4, 0xe2, 0xce, 0x4e,
                           0xc2, 0x78, 0x7a, 0xd0, 0xb3, 0x05, 0x45, 0xe1, 0x7c, 0xde, 0xda,
                           0xa8, 0x33, 0xb7, 0xd6, 0xb8, 0xa7, 0x02, 0x03, 0x8b, 0x27, 0x4e,
                           0xae, 0xa3, 0xf4, 0xe4, 0xbe, 0x9d, 0x91, 0x4e, 0xeb, 0x61, 0xf1,
                           0x70, 0x2e, 0x69, 0x6c, 0x20, 0x3a, 0x12, 0x68, 0x54}},

    // RFC test case 2
    {{0x4a, 0x65, 0x66, 0x65},
     4,
     {0x77, 0x68, 0x61, 0x74, 0x20, 0x64, 0x6f, 0x20, 0x79, 0x61, 0x20, 0x77, 0x61, 0x6e,
      0x74, 0x20, 0x66, 0x6f, 0x72, 0x20, 0x6e, 0x6f, 0x74, 0x68, 0x69, 0x6e, 0x67, 0x3f},
     28,
     SHA512Block::HashType{0x16, 0x4b, 0x7a, 0x7b, 0xfc, 0xf8, 0x19, 0xe2, 0xe3, 0x95, 0xfb,
                           0xe7, 0x3b, 0x56, 0xe0, 0xa3, 0x87, 0xbd, 0x64, 0x22, 0x2e, 0x83,
                           0x1f, 0xd6, 0x10, 0x27, 0x0c, 0xd7, 0xea, 0x25, 0x05, 0x54, 0x97,
                           0x58, 0xbf, 0x75, 0xc0, 0x5a, 0x99, 0x4a, 0x6d, 0x03, 0x4f, 0x65,
                           0xf8, 0xf0, 0xe6, 0xfd, 0xca, 0xea, 0xb1, 0xa3, 0x4d, 0x4a, 0x6b,
                           0x4b, 0x63, 0x6e, 0x07, 0x0a, 0x38, 0xbc, 0xe7, 0x37}},

    // RFC test case 3
    {{0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
      0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa},
     20,
     {0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd,
      0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd,
      0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd,
      0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd},
     50,
     SHA512Block::HashType{0xfa, 0x73, 0xb0, 0x08, 0x9d, 0x56, 0xa2, 0x84, 0xef, 0xb0, 0xf0,
                           0x75, 0x6c, 0x89, 0x0b, 0xe9, 0xb1, 0xb5, 0xdb, 0xdd, 0x8e, 0xe8,
                           0x1a, 0x36, 0x55, 0xf8, 0x3e, 0x33, 0xb2, 0x27, 0x9d, 0x39, 0xbf,
                           0x3e, 0x84, 0x82, 0x79, 0xa7, 0x22, 0xc8, 0x06, 0xb4, 0x85, 0xa4,
                           0x7e, 0x67, 0xc8, 0x07, 0xb9, 0x46, 0xa3, 0x37, 0xbe, 0xe8, 0x94,
                           0x26, 0x74, 0x27, 0x88, 0x59, 0xe1, 0x32, 0x92, 0xfb}},

    // RFC test case 4
    {{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
      0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19},
     25,
     {0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd,
      0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd,
      0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd,
      0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd},
     50,
     SHA512Block::HashType{0xb0, 0xba, 0x46, 0x56, 0x37, 0x45, 0x8c, 0x69, 0x90, 0xe5, 0xa8,
                           0xc5, 0xf6, 0x1d, 0x4a, 0xf7, 0xe5, 0x76, 0xd9, 0x7f, 0xf9, 0x4b,
                           0x87, 0x2d, 0xe7, 0x6f, 0x80, 0x50, 0x36, 0x1e, 0xe3, 0xdb, 0xa9,
                           0x1c, 0xa5, 0xc1, 0x1a, 0xa2, 0x5e, 0xb4, 0xd6, 0x79, 0x27, 0x5c,
                           0xc5, 0x78, 0x80, 0x63, 0xa5, 0xf1, 0x97, 0x41, 0x12, 0x0c, 0x4f,
                           0x2d, 0xe2, 0xad, 0xeb, 0xeb, 0x10, 0xa2, 0x98, 0xdd}},
};

TEST(CryptoVectors, HMACSHA512) {
    size_t numTests = sizeof(hmacSha512Tests) / sizeof(hmacSha512Tests[0]);
    for (size_t i = 0; i < numTests; i++) {
        SHA512Block result = SHA512Block::computeHmac(hmacSha512Tests[i].key,
                                                      hmacSha512Tests[i].keyLen,
                                                      hmacSha512Tests[i].data,
                                                      hmacSha512Tests[i].dataLen);
        ASSERT(hmacSha512Tests[i].hash == result) << "Failed HMAC-SHA512 iteration " << i;
    }
}

TEST(CryptoVectors, HMACSHA512_ContextReuse) {
    size_t numTests = sizeof(hmacSha512Tests) / sizeof(hmacSha512Tests[0]);
    HmacContext ctx;
    for (size_t i = 0; i < numTests; i++) {
        SHA512Block result;
        SHA512Block::computeHmacWithCtx(
            &ctx,
            hmacSha512Tests[i].key,
            hmacSha512Tests[i].keyLen,
            {ConstDataRange(&hmacSha512Tests[i].data[0], hmacSha512Tests[i].dataLen)},
            &result);
        ASSERT(hmacSha512Tests[i].hash == result) << "Failed HMAC-SHA512 iteration " << i;
    }
}

/**
 * This test runs 256 times.
 *
 * Each time, the test randomly generates a key and then generates a random string 256
 * times. It attempts to hmac this same string using the randomly generated key using three
 * different mechanisms - computeHmac, computeHmacWithCtx, and computeHmacWithCtx with key
 * reuse. It asserts that the result is the same every time.
 */
TEST(CryptoVectors, HMACSHA1_KeyReuse) {
    PseudoRandom random(12345);
    for (size_t i = 0; i < 256; i++) {
        const auto key =
            crypto::aesGenerate(crypto::sym256KeySize, "randomKey" + std::to_string(i));
        HmacContext nonKeyReuseContext;
        HmacContext keyReuseContext;

        SHA512Block noReuseResult;
        SHA512Block noKeyReuseResult;
        SHA512Block keyReuseResult;

        keyReuseContext.setReuseKey(true);
        for (size_t j = 0; j < 256; j++) {
            unsigned char data[maxDataSize];
            random.fill(&data, maxDataSize);
            ConstDataRange cdr(data);
            noReuseResult =
                SHA512Block::computeHmac(key.getKey(), key.getKeySize(), data, maxDataSize);
            SHA512Block::computeHmacWithCtx(
                &nonKeyReuseContext, key.getKey(), key.getKeySize(), {cdr}, &noKeyReuseResult);
            SHA512Block::computeHmacWithCtx(
                &keyReuseContext, key.getKey(), key.getKeySize(), {cdr}, &keyReuseResult);
            ASSERT_EQ(noReuseResult, noKeyReuseResult);
            ASSERT_EQ(noKeyReuseResult, keyReuseResult);
        }
    }
}


TEST(SHA512Block, BinDataRoundTrip) {
    SHA512Block::HashType rawHash;
    rawHash.fill(0);
    for (size_t i = 0; i < rawHash.size(); i++) {
        rawHash[i] = i;
    }

    SHA512Block testHash(rawHash);

    BSONObjBuilder builder;
    testHash.appendAsBinData(builder, "hash");
    auto newObj = builder.done();

    auto hashElem = newObj["hash"];
    ASSERT_EQ(BSONType::binData, hashElem.type());
    ASSERT_EQ(BinDataGeneral, hashElem.binDataType());

    int binLen = 0;
    auto rawBinData = hashElem.binData(binLen);
    ASSERT_EQ(SHA512Block::kHashLength, static_cast<size_t>(binLen));

    auto newHashStatus =
        SHA512Block::fromBinData(BSONBinData(rawBinData, binLen, hashElem.binDataType()));
    ASSERT_OK(newHashStatus.getStatus());
    ASSERT_TRUE(testHash == newHashStatus.getValue());
}

TEST(SHA512Block, CanOnlyConstructFromBinGeneral) {
    std::string dummy(SHA512Block::kHashLength, 'x');

    auto newHashStatus =
        SHA512Block::fromBinData(BSONBinData(dummy.c_str(), dummy.size(), newUUID));
    ASSERT_EQ(ErrorCodes::UnsupportedFormat, newHashStatus.getStatus());
}

TEST(SHA512Block, FromBinDataShouldRejectWrongSize) {
    std::string dummy(SHA512Block::kHashLength - 1, 'x');

    auto newHashStatus =
        SHA512Block::fromBinData(BSONBinData(dummy.c_str(), dummy.size(), BinDataGeneral));
    ASSERT_EQ(ErrorCodes::UnsupportedFormat, newHashStatus.getStatus());

    dummy = std::string(SHA512Block::kHashLength - 1, 'x');

    newHashStatus =
        SHA512Block::fromBinData(BSONBinData(dummy.c_str(), dummy.size(), BinDataGeneral));
    ASSERT_EQ(ErrorCodes::UnsupportedFormat, newHashStatus.getStatus());
}

TEST(SHA512Block, FromBufferShouldRejectWrongLength) {
    std::string dummy(SHA512Block::kHashLength - 1, 'x');

    auto newHashStatus =
        SHA512Block::fromBuffer(reinterpret_cast<const uint8_t*>(dummy.c_str()), dummy.size());
    ASSERT_EQ(ErrorCodes::InvalidLength, newHashStatus.getStatus());

    dummy = std::string(SHA512Block::kHashLength + 1, 'x');

    newHashStatus =
        SHA512Block::fromBuffer(reinterpret_cast<const uint8_t*>(dummy.c_str()), dummy.size());
    ASSERT_EQ(ErrorCodes::InvalidLength, newHashStatus.getStatus());
}

}  // namespace
}  // namespace mongo
