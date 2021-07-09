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

#include "mongo/bson/util/bitstream_builder.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

TEST(BitStreamBuilderBSON, AddZeroBits) {
    BitStreamBuilder obj;
    int32_t numBits = 0;
    uint64_t data = 0xA;
    obj.appendBits(data, numBits);
    ASSERT_EQUALS(obj.getBuffer()->len(), 0);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 0);
}

TEST(BitStreamBuilderBSON, AddByte) {
    BitStreamBuilder obj;
    uint64_t data = 0xAB;
    int32_t numBits = 8;
    obj.appendBits(data, numBits);
    ASSERT_EQUALS(obj.getBuffer()->len(), 1);
    ASSERT_EQUALS(*(obj.getBuffer()->buf()), char(0xAB));
    ASSERT_EQUALS(obj.getCurrentBitPos(), 0);
}

TEST(BitStreamBuilderBSON, AddPartialByte) {
    BitStreamBuilder obj;
    uint64_t data = 0xFAB;
    int32_t numBits = 4;
    obj.appendBits(data, numBits);
    ASSERT_EQUALS(*(obj.getBuffer()->buf()), char(0xB));
    ASSERT_EQUALS(obj.getBuffer()->len(), 1);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 4);
}

TEST(BitStreamBuilderBSON, AddFullInt) {
    BitStreamBuilder obj;
    std::vector<unsigned char> testArr = {0xCF, 0xAB, 0xCF, 0xAB};
    uint64_t data = 0xABCFABCF;
    int32_t numBits = sizeof(data) * 8;
    obj.appendBits(data, numBits);
    ASSERT_EQUALS(obj.getBuffer()->len(), 8);
    ASSERT_EQUALS(0, std::memcmp(obj.getBuffer()->buf(), testArr.data(), testArr.size()));
    ASSERT_EQUALS(obj.getCurrentBitPos(), 0);
}

TEST(BitStreamBuilderBSON, AddByteThenPartialByte) {
    BitStreamBuilder obj;
    uint64_t data = 0xAB;
    int32_t numBits = 8;
    obj.appendBits(data, numBits);
    std::vector<unsigned char> testArr = {0xAB};
    ASSERT_EQUALS(0, std::memcmp(obj.getBuffer()->buf(), testArr.data(), testArr.size()));
    ASSERT_EQUALS(obj.getBuffer()->len(), 1);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 0);
    data = 0xC;
    numBits = 4;
    obj.appendBits(data, numBits);
    testArr.push_back(0xC);
    ASSERT_EQUALS(0, std::memcmp(obj.getBuffer()->buf(), testArr.data(), testArr.size()));
    ASSERT_EQUALS(obj.getBuffer()->len(), 2);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 4);
}

TEST(BitStreamBuilderBSON, AddTwoPartialBytes) {
    BitStreamBuilder obj;
    uint64_t data = 0xAB;
    int32_t numBits = 4;
    obj.appendBits(data, numBits);
    ASSERT_EQUALS(*(obj.getBuffer()->buf()), char(0xB));
    ASSERT_EQUALS(obj.getBuffer()->len(), 1);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 4);
    data = 0xA;
    obj.appendBits(data, numBits);
    ASSERT_EQUALS(*(obj.getBuffer()->buf()), char(0xAB));
    ASSERT_EQUALS(obj.getBuffer()->len(), 1);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 0);
}

TEST(BitStreamBuilderBSON, AddTwoPartialBytesAndLeavePartialState) {
    BitStreamBuilder obj;
    uint64_t data = 0xAB;
    int32_t numBits = 3;
    obj.appendBits(data, numBits);
    ASSERT_EQUALS(*(obj.getBuffer()->buf()), char(0x3));
    ASSERT_EQUALS(obj.getBuffer()->len(), 1);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 3);
    data = 0x2;
    numBits = 2;
    obj.appendBits(data, numBits);
    ASSERT_EQUALS(*(obj.getBuffer()->buf()), char(0x13));
    ASSERT_EQUALS(obj.getBuffer()->len(), 1);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 5);
}

TEST(BitStreamBuilderBSON, AddBitsThenAddByte) {
    BitStreamBuilder obj;
    uint64_t data = 0xAB;
    int32_t numBits = 4;
    obj.appendBits(data, numBits);
    std::vector<unsigned char> testArr = {0xB};
    ASSERT_EQUALS(0, std::memcmp(obj.getBuffer()->buf(), testArr.data(), testArr.size()));
    ASSERT_EQUALS(obj.getBuffer()->len(), 1);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 4);
    data = 0xCD;
    numBits = 8;
    obj.appendBits(data, numBits);
    testArr = {0xDB, 0xC};
    ASSERT_EQUALS(0, std::memcmp(obj.getBuffer()->buf(), testArr.data(), testArr.size()));
    ASSERT_EQUALS(obj.getBuffer()->len(), 2);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 4);
}

TEST(BitStreamBuilderBSON, AddBitsThenAddBits) {
    BitStreamBuilder obj;
    uint64_t data = 0xFB;
    int32_t numBits = 5;
    obj.appendBits(data, numBits);
    std::vector<unsigned char> testArr = {0x1B};
    ASSERT_EQUALS(0, std::memcmp(obj.getBuffer()->buf(), testArr.data(), testArr.size()));
    ASSERT_EQUALS(obj.getBuffer()->len(), 1);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 5);
    // only add bits in parenthesis to buffer
    // CD == 1100(1101) + 000(11011)  = 1 10111011
    data = 0xCD;
    numBits = 4;
    obj.appendBits(data, numBits);
    testArr = {0xBB, 0x1};
    ASSERT_EQUALS(0, std::memcmp(obj.getBuffer()->buf(), testArr.data(), testArr.size()));
    ASSERT_EQUALS(obj.getBuffer()->len(), 2);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 1);
}

TEST(BitStreamBuilderBSON, AddBitsAddByteAddBits) {
    BitStreamBuilder obj;
    uint64_t data = 0xFB;
    int32_t numBits = 6;
    obj.appendBits(data, numBits);
    std::vector<unsigned char> testArr = {0x3B};
    ASSERT_EQUALS(0, std::memcmp(obj.getBuffer()->buf(), testArr.data(), testArr.size()));
    ASSERT_EQUALS(obj.getBuffer()->len(), 1);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 6);
    numBits = 8;
    // only add bits in parenthesis to buffer
    // FB := (11111011) + 00(111011) = 00111110 11111011
    obj.appendBits(data, numBits);
    testArr = {0xFB, 0x3E};
    ASSERT_EQUALS(0, std::memcmp(obj.getBuffer()->buf(), testArr.data(), testArr.size()));
    ASSERT_EQUALS(obj.getBuffer()->len(), 2);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 6);
    // only add bits in parenthesis to buffer
    // CD := 1100(1101) + 00(111110 11111011)  = 00000011 01111110 11111011
    data = 0xCD;
    numBits = 4;
    obj.appendBits(data, numBits);
    testArr = {0xFB, 0x7E, 0x3};
    ASSERT_EQUALS(0, std::memcmp(obj.getBuffer()->buf(), testArr.data(), testArr.size()));
    ASSERT_EQUALS(obj.getBuffer()->len(), 3);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 2);
}

TEST(BitStreamBuilderBSON, AddBitsAddByteAddBitsAddTwoByte) {
    BitStreamBuilder obj;
    uint64_t data = 0x3B;
    int32_t numBits = 6;
    obj.appendBits(data, numBits);
    std::vector<unsigned char> testArr = {0x3B};
    ASSERT_EQUALS(0, std::memcmp(obj.getBuffer()->buf(), testArr.data(), testArr.size()));
    ASSERT_EQUALS(obj.getBuffer()->len(), 1);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 6);
    data = 0xFB;
    numBits = 8;
    // only add bits in parenthesis to buffer
    // FB := (11111011) + 00(111011) = 00111110 11111011
    obj.appendBits(data, numBits);
    testArr = {0xFB, 0x3E};
    ASSERT_EQUALS(0, std::memcmp(obj.getBuffer()->buf(), testArr.data(), testArr.size()));
    ASSERT_EQUALS(obj.getBuffer()->len(), 2);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 6);
    // only add bits in parenthesis to buffer
    // CD := 1100(1101) + 00(111110 11111011)  = 00000011 01111110 11111011
    data = 0xCD;
    numBits = 4;
    obj.appendBits(data, numBits);
    testArr = {0xFB, 0x7E, 0x3};
    ASSERT_EQUALS(0, std::memcmp(obj.getBuffer()->buf(), testArr.data(), testArr.size()));
    ASSERT_EQUALS(obj.getBuffer()->len(), 3);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 2);
    data = 0xCCAA;
    numBits = 16;
    obj.appendBits(data, numBits);
    // only add bits in parenthesis to buffer
    // AA := (10101010)  -> 000000(10 10101011 01111110 11111011)
    // CC := (11001100)  -> 000000(11 00110010 10101011 01111110 11111011)
    testArr[2] = 0xAB;
    testArr = {0xFB, 0x7E, 0xAB, 0x32, 0x3};
    ASSERT_EQUALS(0, std::memcmp(obj.getBuffer()->buf(), testArr.data(), testArr.size()));
    ASSERT_EQUALS(obj.getBuffer()->len(), 5);
    ASSERT_EQUALS(obj.getCurrentBitPos(), 2);
}
