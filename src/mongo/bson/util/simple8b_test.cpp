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

#include <vector>

#include "mongo/bson/util/simple8b.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;


void assertVectorsEqual(const std::vector<Simple8b::Value>& actualVector,
                        const std::vector<Simple8b::Value>& expectedVector) {
    ASSERT_EQ(actualVector.size(), expectedVector.size());

    for (size_t i = 0; i < actualVector.size(); ++i) {
        ASSERT_EQ(actualVector[i].val, expectedVector[i].val);
        ASSERT_EQ(actualVector[i].index, expectedVector[i].index);
    }
}

void testAppendAndGetAllInts(Simple8b& s8b, const std::vector<uint64_t>& expectedInts) {
    std::vector<Simple8b::Value> expectedValues{};

    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

void testFlush(Simple8b& s8b, const std::vector<uint8_t>& expectedChar) {
    s8b.flush();

    char* hex = s8b.data();
    size_t len = s8b.len();
    ASSERT_EQ(len, expectedChar.size());

    for (size_t i = 0; i < len; ++i) {
        ASSERT_EQ(static_cast<uint8_t>(*hex), expectedChar[i]) << i;
        ++hex;
    }
}

TEST(Simple8b, NoValues) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts{};
    testAppendAndGetAllInts(s8b, expectedInts);

    s8b.flush();
    size_t len = s8b.len();
    std::vector<uint8_t> expectedChar{};
    ASSERT_EQ(len, expectedChar.size());
}

TEST(Simple8b, OnlySkip) {
    Simple8b s8b;

    s8b.skip();
    std::vector<Simple8b::Value> values = s8b.getAllInts();
    std::vector<Simple8b::Value> expectedValues{};

    assertVectorsEqual(values, expectedValues);

    // The selector is 14 and the remaining 60 bits of data are all 1s, which represents skip.
    std::vector<uint8_t> expectedChar{0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // 1st word.
    testFlush(s8b, expectedChar);
}

TEST(Simple8b, OneValuePending) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts = {1};
    testAppendAndGetAllInts(s8b, expectedInts);

    // The selector is 14 and there is only 1 bucket with the value 1.
    std::vector<uint8_t> expectedChar{0x1E, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};  // 1st word.
    testFlush(s8b, expectedChar);
}

TEST(Simple8b, MaxValuePending) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts = {0xFFFFFFFFFFFFFFE};
    testAppendAndGetAllInts(s8b, expectedInts);

    // The selector is 14 and there is only 1 bucket with the max possible value 0xFFFFFFFFFFFFFFE.
    std::vector<uint8_t> expectedChar{0xEE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // 1st word.
    testFlush(s8b, expectedChar);
}

TEST(Simple8b, MultipleValuesPending) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts = {1, 2, 3};
    testAppendAndGetAllInts(s8b, expectedInts);

    // The selector is 12 and there are 3 bucket with the values 1, 2 and 3.
    std::vector<uint8_t> expectedChar{0x1C, 0x0, 0x0, 0x2, 0x0, 0x30, 0x0, 0x0};  // 1st word.
    testFlush(s8b, expectedChar);
}

TEST(Simple8b, MaxValuesPending) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(60, 1);
    testAppendAndGetAllInts(s8b, expectedInts);

    // The selector is 2 and there are 30 bucket with the same value 0b01. 0x55 = 0b01010101.
    std::vector<uint8_t> expectedChar{
        0x52,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,  // 1st word.
        0x52,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55  // 2nd word.
    };
    testFlush(s8b, expectedChar);
}

TEST(Simple8b, EncodeWithTrailingDirtyBits) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(7, 1);
    testAppendAndGetAllInts(s8b, expectedInts);

    // The selector is 8 and there are 7 bucket with the same value 0b00000001.
    // The last 4 bits are dirty/unused.
    std::vector<uint8_t> expectedChar{0x18, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0};  // 1st word.
    testFlush(s8b, expectedChar);
}

TEST(Simple8b, FullBufferAndPending) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(120, 1);
    testAppendAndGetAllInts(s8b, expectedInts);

    // The selector is 2 and there are 30 bucket with the same value 0b01. 0x55 = 0b01010101.
    std::vector<uint8_t> expectedChar{
        0x52, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,  // 1st word.
        0x52, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,  // 2nd word.
        0x52, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,  // 3rd word.
        0x52, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55   // 4th word.
    };
    testFlush(s8b, expectedChar);
}

TEST(Simple8b, TwoFullBuffersAndPending) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(180, 1);
    testAppendAndGetAllInts(s8b, expectedInts);

    // The selector is 2 and there are 30 bucket with the same value 0b01. 0x55 = 0b01010101.
    std::vector<uint8_t> expectedChar{
        0x52, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,  // 1st word.
        0x52, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,  // 2nd word.
        0x52, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,  // 3rd word.
        0x52, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,  // 4th word.
        0x52, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,  // 5th word.
        0x52, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55   // 6th word.
    };
    testFlush(s8b, expectedChar);
}

TEST(Simple8b, MaxValueBuffer) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(3, 0xFFFFFFFFFFFFFFE);
    testAppendAndGetAllInts(s8b, expectedInts);

    // The selector is 14 and there is only 1 bucket with the max possible value 0xFFFFFFFFFFFFFFE.
    std::vector<uint8_t> expectedChar{
        0xEE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 1st word.
        0xEE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 2nd word.
        0xEE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF   // 3rd word.
    };
    testFlush(s8b, expectedChar);
}

TEST(Simple8b, TrySomeSmallValuesWithoutFlush) {
    Simple8b s8b;

    std::vector<Simple8b::Value> expectedValues;
    for (size_t num = 0; num <= 0x0001FFFFF; ++num) {
        expectedValues.push_back({(uint32_t)num, num});
        ASSERT_TRUE(s8b.append(num));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, TrySomeLargeValuesWithoutFlush) {
    Simple8b s8b;

    std::vector<Simple8b::Value> expectedValues;
    for (size_t num = 0xF00000000; num <= 0xF001FFFFF; ++num) {
        expectedValues.push_back({(uint32_t)num, num});
        ASSERT_TRUE(s8b.append(num));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, BreakPendingIntoMultipleSimple8bBlocks) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(57, 1);
    expectedInts.push_back(15);
    // 15 is 0b1111 and can not be added to the current word because it would overflow.
    // We can not form a 57 bit word because we would be unable to determine
    // if the last 3 bits are empty or unused.
    // Therefore, we must form a word with 30 integers of 1's, 20 integers of 1's
    // and the current vector would have seven 1's and one 15.
    testAppendAndGetAllInts(s8b, expectedInts);

    std::vector<uint8_t> expectedChar{
        // The selector is 2 and there are 30 bucket with the same value 0b01. 0x55 = 0b01010101.
        0x52,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,  // 1st word.
               // The selector is 3 and there are 20 bucket with the same value 0b01.
               // 0x24 = 0b00100100, 0x49 = 0b01001001 and 0x92 = 0b10010010.
        0x93,
        0x24,
        0x49,
        0x92,
        0x24,
        0x49,
        0x92,
        0x24,  // 2nd word.
        // The selector is 7 and there are 8 bucket of 0b01 except the last bucket which is 0b1111.
        // 0xE0 = 0b11100000 and 0x1 = 0x00000001 and together the last bucket is 0b1111.
        0x17,
        0x8,
        0x4,
        0x2,
        0x81,
        0x40,
        0xE0,
        0x1  // 3rd word.
    };
    testFlush(s8b, expectedChar);
}

TEST(Simple8b, BreakPendingValuesIntoMultipleSimple8bWords) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(50, 0);
    expectedInts.push_back(0xFFFFFFFFFFFF);  // 48 bit value.
    // 0xFFFFFFFFFFFF is 48 bits and can not be added to the current word because it would overflow.
    // We can not form a 57 bit word because we would be unable to determine
    // if the last 3 bits are empty or unused. Therefore, we must form a word with 30 integers
    // of 0's and 20 integers of 0's in the same append() iteration.
    testAppendAndGetAllInts(s8b, expectedInts);

    std::vector<uint8_t> expectedChar{
        // The selector is 2 and there are 30 bucket with the same value 0b00.
        0x2,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,  // 1st word.
              // The selector is 3 and there are 20 bucket with the same value 0b00.
        0x3,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,  // 2nd word.
              // The selector is 14 and there is only 1 bucket with 0xFFFFFFFFFFFF.
        0xFE,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xF,
        0x0  // 3rd word.
    };
    testFlush(s8b, expectedChar);
}

TEST(Simple8b, SkipInPending) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(3, 3);
    std::vector<Simple8b::Value> expectedValues;
    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    s8b.skip();
    int index = expectedInts.size() + 1;
    expectedValues.push_back({(uint32_t)index, 7});
    ASSERT_TRUE(s8b.append(expectedValues.back().val));

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);

    // The selector is 10 and there are 5 bucket with 12 bit buckets.
    // 0xFF and 0x7F is 15 1's. The skip is 12 1's and 7 is 3 1's.
    std::vector<uint8_t> expectedChar{0x3A, 0x0, 0x3, 0x30, 0x0, 0xFF, 0x7F, 0x0};  // 1st word.
    testFlush(s8b, expectedChar);
}

TEST(Simple8b, SkipInBuf) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(50, 1);
    std::vector<Simple8b::Value> expectedValues;
    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    s8b.skip();

    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)(i + expectedInts.size() + 1), expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);

    std::vector<uint8_t> expectedChar{
        // The selector is 2 and there are 30 bucket with the same value 0b01. 0x55 = 0b01010101.
        0x52,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,  // 1st word.
               // The selector is 2 and there are 30 bucket with the same value 0b01
               // except the skip, which is the 0x7 in the 6th byte. 0x55 = 0b01010101.
        0x52,
        0x55,
        0x55,
        0x55,
        0x55,
        0x75,
        0x55,
        0x55,  // 2nd word.
        // The selector is 2 and there are 30 bucket with the same value 0b01. 0x55 = 0b01010101.
        0x52,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,  // 3rd word.
               // The selector is 6 and there are 10 bucket with the same value 0b000001.
        0x16,
        0x4,
        0x41,
        0x10,
        0x4,
        0x41,
        0x10,
        0x4,  // 4th word.
              // The selector is 14 and there is only 1 bucket with the value 1.
        0x1E,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0  // 5th word.
    };
    testFlush(s8b, expectedChar);
}

TEST(Simple8b, TrailingSkipsDoNotShowUp) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(48, 1);
    std::vector<Simple8b::Value> expectedValues;
    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    s8b.skip();
    s8b.skip();

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);

    std::vector<uint8_t> expectedChar{
        // The selector is 2 and there are 30 bucket with the same value 0b01. 0x55 = 0b01010101.
        0x52,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,  // 1st word.
               // The selector is 3 and there are 20 bucket with the same value 0b001.
        // except the last 2 buckets, which are skips. 0xFC = 11111100, which is exactly 2 skips.
        0x93,
        0x24,
        0x49,
        0x92,
        0x24,
        0x49,
        0x92,
        0xFC  // 2nd word.
    };
    testFlush(s8b, expectedChar);
}

TEST(Simple8b, LeadingSkips) {
    Simple8b s8b;

    int numSkips = 2;
    s8b.skip();
    s8b.skip();

    std::vector<uint64_t> expectedInts = {3, 8, 13};
    std::vector<Simple8b::Value> expectedValues;
    for (size_t i = 0; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i + numSkips, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);

    // The selector is 10 and there are 5 bucket with 12 bit buckets.
    // The first two buckets are skips.
    std::vector<uint8_t> expectedChar{0xFA, 0xFF, 0xFF, 0x3F, 0x0, 0x8, 0xD0, 0x0};  // 1st word.
    testFlush(s8b, expectedChar);
}

TEST(Simple8b, WordOfSkips) {
    Simple8b s8b;

    int numSkips = 30;
    for (int i = 0; i < numSkips; ++i)
        s8b.skip();

    uint64_t numWithMoreThanThirtyBits = 1ull << 30;
    std::vector<uint64_t> expectedInts = {numWithMoreThanThirtyBits};
    std::vector<Simple8b::Value> expectedValues;
    expectedValues.push_back({(uint32_t)numSkips, numWithMoreThanThirtyBits});
    ASSERT_TRUE(s8b.append(numWithMoreThanThirtyBits));

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);

    std::vector<uint8_t> expectedChar{
        // The selector is 2 and there are 30 bucket with the same value 0b11, referring to skip.
        0xF2,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,  // 1st word.
               // The selector is 14 and there is only bucket with the value
               // 0b100000000000000000000000000000.
        0xE,
        0x0,
        0x0,
        0x0,
        0x4,
        0x0,
        0x0,
        0x0  // 2nd word.
    };
    testFlush(s8b, expectedChar);
}

TEST(Simple8b, MultipleFlushes) {
    Simple8b s8b;

    std::vector<uint64_t> values = {1};
    for (size_t i = 0; i < values.size(); ++i) {
        ASSERT_TRUE(s8b.append(values[i]));
    }

    s8b.flush();

    values[0] = 2;
    for (size_t i = 0; i < values.size(); ++i) {
        ASSERT_TRUE(s8b.append(values[i]));
    }

    std::vector<uint8_t> expectedChar{
        // The selector is 14 and there is only 1 bucket with the value 1.
        0x1E,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,  // 1st word.
              // The selector is 14 and there is only 1 bucket with the value 2.
        0x2E,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0  // 2nd word.
    };
    testFlush(s8b, expectedChar);
}
