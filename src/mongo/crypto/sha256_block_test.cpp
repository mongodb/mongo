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

#include "mongo/crypto/sha256_block.h"

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

// SHA-256 test vectors from tom crypt
const struct {
    std::initializer_list<ConstDataRange> msg;
    SHA256Block hash;
} sha256Tests[] = {
    {{makeTestItem("abc")},
     SHA256Block::HashType{0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                           0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                           0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad}},

    {{makeTestItem("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")},
     SHA256Block::HashType{0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26,
                           0x93, 0x0c, 0x3e, 0x60, 0x39, 0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff,
                           0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1}},

    // A variation on the above to test digesting multiple parts
    {{makeTestItem("abcdbcdecdefdefgefghfghi"), makeTestItem("ghijhijkijkljklmklmnlmnomnopnopq")},
     SHA256Block::HashType{0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26,
                           0x93, 0x0c, 0x3e, 0x60, 0x39, 0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff,
                           0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1}}};


TEST(CryptoVectors, SHA256) {
    size_t numTests = sizeof(sha256Tests) / sizeof(sha256Tests[0]);
    SHA256Block::Secure resultSec;
    for (size_t i = 0; i < numTests; i++) {
        // Normal allocator.
        SHA256Block result = SHA256Block::computeHash(sha256Tests[i].msg);
        ASSERT_EQ(sha256Tests[i].hash, result) << "Failed SHA256 iteration " << i;
        // Secure allocator.
        SHA256Block::Secure::computeHash(sha256Tests[i].msg, &resultSec);
        ASSERT_EQ(sha256Tests[i].hash, resultSec) << "Failed SHA256 secure iteration " << i;

        ASSERT_EQ(result, resultSec) << "Stack allocator != Secure allocator hash" << i;
    }
}

const int maxKeySize = 80;
const int maxDataSize = 54;
// HMAC-SHA-256 test vectors from https://tools.ietf.org/html/rfc4231#section-4.2
const struct {
    unsigned char key[maxKeySize];
    int keyLen;
    unsigned char data[maxDataSize];
    int dataLen;
    SHA256Block hash;
} hmacSha256Tests[] = {
    // RFC test case 1
    {{0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
      0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b},
     20,
     {0x48, 0x69, 0x20, 0x54, 0x68, 0x65, 0x72, 0x65},
     8,
     SHA256Block::HashType{0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf,
                           0xce, 0xaf, 0x0b, 0xf1, 0x2b, 0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83,
                           0x3d, 0xa7, 0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7}},

    // RFC test case 2
    {{0x4a, 0x65, 0x66, 0x65},
     4,
     {0x77, 0x68, 0x61, 0x74, 0x20, 0x64, 0x6f, 0x20, 0x79, 0x61, 0x20, 0x77, 0x61, 0x6e,
      0x74, 0x20, 0x66, 0x6f, 0x72, 0x20, 0x6e, 0x6f, 0x74, 0x68, 0x69, 0x6e, 0x67, 0x3f},
     28,
     SHA256Block::HashType{0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24,
                           0x26, 0x08, 0x95, 0x75, 0xc7, 0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27,
                           0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43}},

    // RFC test case 3
    {{0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
      0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa},
     20,
     {0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd,
      0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd,
      0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd,
      0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd},
     50,
     SHA256Block::HashType{0x77, 0x3e, 0xa9, 0x1e, 0x36, 0x80, 0x0e, 0x46, 0x85, 0x4d, 0xb8,
                           0xeb, 0xd0, 0x91, 0x81, 0xa7, 0x29, 0x59, 0x09, 0x8b, 0x3e, 0xf8,
                           0xc1, 0x22, 0xd9, 0x63, 0x55, 0x14, 0xce, 0xd5, 0x65, 0xfe}},

    // RFC test case 4
    {{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
      0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19},
     25,
     {0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd,
      0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd,
      0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd,
      0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd},
     50,
     SHA256Block::HashType{0x82, 0x55, 0x8a, 0x38, 0x9a, 0x44, 0x3c, 0x0e, 0xa4, 0xcc, 0x81,
                           0x98, 0x99, 0xf2, 0x08, 0x3a, 0x85, 0xf0, 0xfa, 0xa3, 0xe5, 0x78,
                           0xf8, 0x07, 0x7a, 0x2e, 0x3f, 0xf4, 0x67, 0x29, 0x66, 0x5b}},
};

TEST(CryptoVectors, HMACSHA256) {
    size_t numTests = sizeof(hmacSha256Tests) / sizeof(hmacSha256Tests[0]);
    for (size_t i = 0; i < numTests; i++) {
        SHA256Block result = SHA256Block::computeHmac(hmacSha256Tests[i].key,
                                                      hmacSha256Tests[i].keyLen,
                                                      hmacSha256Tests[i].data,
                                                      hmacSha256Tests[i].dataLen);
        ASSERT(hmacSha256Tests[i].hash == result) << "Failed HMAC-SHA256 iteration " << i;
    }
}

TEST(CryptoVectors, HMACSHA256_ContextReuse) {
    size_t numTests = sizeof(hmacSha256Tests) / sizeof(hmacSha256Tests[0]);
    HmacContext ctx;
    for (size_t i = 0; i < numTests; i++) {
        SHA256Block result;
        SHA256Block::computeHmacWithCtx(
            &ctx,
            hmacSha256Tests[i].key,
            hmacSha256Tests[i].keyLen,
            {ConstDataRange(&hmacSha256Tests[i].data[0], hmacSha256Tests[i].dataLen)},
            &result);
        ASSERT(hmacSha256Tests[i].hash == result) << "Failed HMAC-SHA256 iteration " << i;
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
TEST(CryptoVectors, HMACSHA256_KeyReuse) {
    PseudoRandom random(12345);
    for (size_t i = 0; i < 256; i++) {
        const auto key =
            crypto::aesGenerate(crypto::sym256KeySize, "randomKey" + std::to_string(i));
        HmacContext nonKeyReuseContext;
        HmacContext keyReuseContext;

        SHA256Block noReuseResult;
        SHA256Block noKeyReuseResult;
        SHA256Block keyReuseResult;

        keyReuseContext.setReuseKey(true);
        for (size_t j = 0; j < 256; j++) {
            unsigned char data[maxDataSize];
            random.fill(&data, maxDataSize);
            ConstDataRange cdr(data);
            noReuseResult =
                SHA256Block::computeHmac(key.getKey(), key.getKeySize(), data, maxDataSize);
            SHA256Block::computeHmacWithCtx(
                &nonKeyReuseContext, key.getKey(), key.getKeySize(), {cdr}, &noKeyReuseResult);
            SHA256Block::computeHmacWithCtx(
                &keyReuseContext, key.getKey(), key.getKeySize(), {cdr}, &keyReuseResult);
            ASSERT_EQ(noReuseResult, noKeyReuseResult);
            ASSERT_EQ(noKeyReuseResult, keyReuseResult);
        }
    }
}

TEST(SHA256Block, BinDataRoundTrip) {
    SHA256Block::HashType rawHash;
    rawHash.fill(0);
    for (size_t i = 0; i < rawHash.size(); i++) {
        rawHash[i] = i;
    }

    SHA256Block testHash(rawHash);

    BSONObjBuilder builder;
    testHash.appendAsBinData(builder, "hash");
    auto newObj = builder.done();

    auto hashElem = newObj["hash"];
    ASSERT_EQ(BSONType::binData, hashElem.type());
    ASSERT_EQ(BinDataGeneral, hashElem.binDataType());

    int binLen = 0;
    auto rawBinData = hashElem.binData(binLen);
    ASSERT_EQ(SHA256Block::kHashLength, static_cast<size_t>(binLen));

    auto newHashStatus =
        SHA256Block::fromBinData(BSONBinData(rawBinData, binLen, hashElem.binDataType()));
    ASSERT_OK(newHashStatus.getStatus());
    ASSERT_TRUE(testHash == newHashStatus.getValue());
}

TEST(SHA256Block, CanOnlyConstructFromBinGeneral) {
    std::string dummy(SHA256Block::kHashLength, 'x');

    auto newHashStatus =
        SHA256Block::fromBinData(BSONBinData(dummy.c_str(), dummy.size(), newUUID));
    ASSERT_EQ(ErrorCodes::UnsupportedFormat, newHashStatus.getStatus());
}

TEST(SHA256Block, FromBinDataShouldRegectWrongSize) {
    std::string dummy(SHA256Block::kHashLength - 1, 'x');

    auto newHashStatus =
        SHA256Block::fromBinData(BSONBinData(dummy.c_str(), dummy.size(), BinDataGeneral));
    ASSERT_EQ(ErrorCodes::UnsupportedFormat, newHashStatus.getStatus());
}

TEST(SHA256Block, FromBufferShouldRejectWrongLength) {
    std::string dummy(SHA256Block::kHashLength - 1, 'x');

    auto newHashStatus =
        SHA256Block::fromBuffer(reinterpret_cast<const uint8_t*>(dummy.c_str()), dummy.size());
    ASSERT_EQ(ErrorCodes::InvalidLength, newHashStatus.getStatus());
}


}  // namespace
}  // namespace mongo
