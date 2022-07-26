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

#include "mongo/platform/basic.h"

#include "kms.h"

#include "mongo/base/data_range.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

bool isEquals(ConstDataRange left, ConstDataRange right) {
    return std::equal(
        left.data(), left.data() + left.length(), right.data(), right.data() + right.length());
}


// Negative: incorrect key size
TEST(KmsTest, TestBadKey) {
    std::array<uint8_t, 3> key{0x1, 0x2, 0x3};
    BSONObj config =
        BSON("local" << BSON("key" << BSONBinData(key.data(), key.size(), BinDataGeneral)));

    ASSERT_THROWS(KMSServiceController::createFromClient("local", config), AssertionException);
}

// Positive: Test Encrypt works
TEST(KmsTest, TestGoodKey) {
    std::array<uint8_t, 96> key = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
        0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b,
        0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
        0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
        0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45,
        0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0x53,
        0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f};

    BSONObj config =
        BSON("local" << BSON("key" << BSONBinData(key.data(), key.size(), BinDataGeneral)));

    auto service = KMSServiceController::createFromClient("local", config);

    auto myKey = "My Secret Key"_sd;

    auto material =
        service->encryptDataKeyByString(ConstDataRange(myKey.rawData(), myKey.size()), "");

    LocalMasterKeyAndMaterial glob =
        LocalMasterKeyAndMaterial::parse(IDLParserContext("root"), material);

    auto keyMaterial = glob.getKeyMaterial();

    auto plaintext = service->decrypt(keyMaterial, BSONObj());

    ASSERT_TRUE(isEquals(myKey.toString(), *plaintext));
}

}  // namespace
}  // namespace mongo
