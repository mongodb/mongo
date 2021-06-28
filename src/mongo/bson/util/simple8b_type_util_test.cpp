/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/util/simple8b_type_util.h"
#include "mongo/unittest/unittest.h"
#include <limits>

using namespace mongo;

TEST(Simple8bTypeUtil, EncodeAndDecodePositiveSignedInt) {
    int64_t signedVal = 1;
    uint64_t unsignedVal = Simple8bTypeUtil::encodeInt64(signedVal);
    ASSERT_EQUALS(unsignedVal, 2);
    int64_t decodedSignedVal = Simple8bTypeUtil::decodeInt64(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}

TEST(Simple8bTypeUtil, EncodeAndDecodeNegativeSignedInt) {
    int64_t signedVal = -1;
    uint64_t unsignedVal = Simple8bTypeUtil::encodeInt64(signedVal);
    ASSERT_EQUALS(unsignedVal, 0x1);
    int64_t decodedSignedVal = Simple8bTypeUtil::decodeInt64(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}

TEST(Simple8bTypeUtil, EncodeAndDecodeMaxPositiveSignedInt) {
    int64_t signedVal = std::numeric_limits<int64_t>::max();
    uint64_t unsignedVal = Simple8bTypeUtil::encodeInt64(signedVal);
    ASSERT_EQUALS(unsignedVal, 0xFFFFFFFFFFFFFFFE);
    int64_t decodedSignedVal = Simple8bTypeUtil::decodeInt64(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}

TEST(Simple8bTypeUtil, EncodeAndDecodeMaxNegativeSignedInt) {
    int64_t signedVal = std::numeric_limits<int64_t>::lowest();
    uint64_t unsignedVal = Simple8bTypeUtil::encodeInt64(signedVal);
    ASSERT_EQUALS(unsignedVal, 0xFFFFFFFFFFFFFFFF);
    int64_t decodedSignedVal = Simple8bTypeUtil::decodeInt64(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}

TEST(Simple8bTypeUtil, EncodeAndDecodeZero) {
    int64_t signedVal = 0;
    uint64_t unsignedVal = Simple8bTypeUtil::encodeInt64(signedVal);
    ASSERT_EQUALS(unsignedVal, 0x0);
    int64_t decodedSignedVal = Simple8bTypeUtil::decodeInt64(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}

TEST(Simple8bTypeUtil, EncodeAndDecodePositiveInt) {
    int64_t signedVal = 1234;
    // Represented as 1234 = 0...010011010010
    //  Left Shifted 0..0100110100100
    //  Right shifted 0..000000000000
    //  xor = 0..0100110100100 = 0x9A4
    uint64_t unsignedVal = Simple8bTypeUtil::encodeInt64(signedVal);
    ASSERT_EQUALS(unsignedVal, 0x9A4);
    int64_t decodedSignedVal = Simple8bTypeUtil::decodeInt64(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}

TEST(Simple8bTypeUtil, EncodeAndDecodeNegativeInt) {
    int64_t signedVal = -1234;
    // Represented as 1234 = 1...010011010010
    //  Left Shifted 0..0100110100100
    //  Right shifted 0..000000000001
    //  xor = 0..0100110100101 = 0x9A3
    uint64_t unsignedVal = Simple8bTypeUtil::encodeInt64(signedVal);
    ASSERT_EQUALS(unsignedVal, 0x9A3);
    int64_t decodedSignedVal = Simple8bTypeUtil::decodeInt64(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}
