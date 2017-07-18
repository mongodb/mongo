/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

ConstDataRange makeTestItem(StringData sd) {
    return ConstDataRange(sd.rawData(), sd.size());
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
    for (size_t i = 0; i < numTests; i++) {
        SHA256Block result = SHA256Block::computeHash(sha256Tests[i].msg);
        ASSERT(sha256Tests[i].hash == result) << "Failed SHA256 iteration " << i;
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
    ASSERT_EQ(BinData, hashElem.type());
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
