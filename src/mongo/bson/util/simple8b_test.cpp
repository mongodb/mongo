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

void testUnderlyingBufferWithFlush(Simple8b& s8b, const std::vector<uint8_t>& expectedChar) {
    s8b.flush();

    char* hex = s8b.data();
    size_t len = s8b.len();
    ASSERT_EQ(len, expectedChar.size());

    for (size_t i = 0; i < len; ++i) {
        ASSERT_EQ(static_cast<uint8_t>(*hex), expectedChar[i]) << i;
        ++hex;
    }
}

void testUnderlyingBufferWithoutFlush(Simple8b& s8b, const std::vector<uint8_t>& expectedChar) {
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
    testUnderlyingBufferWithFlush(s8b, expectedChar);
}

TEST(Simple8b, OneValuePending) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts = {1};
    testAppendAndGetAllInts(s8b, expectedInts);

    // The selector is 14 and there is only 1 bucket with the value 1.
    std::vector<uint8_t> expectedChar{0x1E, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};  // 1st word.
    testUnderlyingBufferWithFlush(s8b, expectedChar);
}

TEST(Simple8b, MaxValuePending) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts = {0xFFFFFFFFFFFFFFE};
    testAppendAndGetAllInts(s8b, expectedInts);

    // The selector is 14 and there is only 1 bucket with the max possible value 0xFFFFFFFFFFFFFFE.
    std::vector<uint8_t> expectedChar{0xEE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // 1st word.
    testUnderlyingBufferWithFlush(s8b, expectedChar);
}

TEST(Simple8b, MultipleValuesPending) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts = {1, 2, 3};
    testAppendAndGetAllInts(s8b, expectedInts);

    // The selector is 12 and there are 3 bucket with the values 1, 2 and 3.
    std::vector<uint8_t> expectedChar{0x1C, 0x0, 0x0, 0x2, 0x0, 0x30, 0x0, 0x0};  // 1st word.
    testUnderlyingBufferWithFlush(s8b, expectedChar);
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
    testUnderlyingBufferWithFlush(s8b, expectedChar);
}

TEST(Simple8b, EncodeWithTrailingDirtyBits) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(7, 1);
    testAppendAndGetAllInts(s8b, expectedInts);

    // The selector is 8 and there are 7 bucket with the same value 0b00000001.
    // The last 4 bits are dirty/unused.
    std::vector<uint8_t> expectedChar{0x18, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0};  // 1st word.
    testUnderlyingBufferWithFlush(s8b, expectedChar);
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
    testUnderlyingBufferWithFlush(s8b, expectedChar);
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
    testUnderlyingBufferWithFlush(s8b, expectedChar);
}

TEST(Simple8b, TrySomeSmallValuesWithoutFlush) {
    Simple8b s8b;

    std::vector<Simple8b::Value> expectedValues;

    for (size_t num = 0; num <= 0x0001FFFF; ++num) {
        expectedValues.push_back({(uint32_t)num, num});
        ASSERT_TRUE(s8b.append(num));
    }
    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, TrySomeLargeValuesWithoutFlush) {
    Simple8b s8b;

    std::vector<Simple8b::Value> expectedValues;
    for (size_t num = 0xF00000000; num <= 0xF0001FFFF; ++num) {
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
    testUnderlyingBufferWithFlush(s8b, expectedChar);
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
    testUnderlyingBufferWithFlush(s8b, expectedChar);
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
    testUnderlyingBufferWithFlush(s8b, expectedChar);
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
    testUnderlyingBufferWithFlush(s8b, expectedChar);
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
    testUnderlyingBufferWithFlush(s8b, expectedChar);
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
    testUnderlyingBufferWithFlush(s8b, expectedChar);
}

TEST(Simple8b, WordOfSkips) {
    Simple8b s8b;

    int numSkips = 30;
    for (int i = 0; i < numSkips; ++i)
        s8b.skip();

    // Add 1 so we don't invoke selector 8 extensions.
    uint64_t numWithMoreThanThirtyBits = (1ull << 30) + 1;
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
        0x1E,
        0x0,
        0x0,
        0x0,
        0x4,
        0x0,
        0x0,
        0x0  // 2nd word.
    };
    testUnderlyingBufferWithFlush(s8b, expectedChar);
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
    testUnderlyingBufferWithFlush(s8b, expectedChar);
}

TEST(Simple8b, Selector7BaseTest) {
    Simple8b s8b;
    // 57344 = 1110000000000000 = 3 value bits and 13 zeros
    // This should be encoded as:
    // [0010] [(111) (1101)] x 8 [0111] = 602C00B0607D81F7
    // This is in hex: 2FBF7EFDFBF7EFD7
    uint64_t val = 57344;
    std::vector<Simple8b::Value> expectedValues;
    for (uint32_t i = 0; i < 9; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i, val});
    }
    // test that buffer was correct
    std::vector<uint8_t> expectedChar = {0xD7, 0xEF, 0xF7, 0xFB, 0xFD, 0x7E, 0xBF, 0x2F};
    testUnderlyingBufferWithoutFlush(s8b, expectedChar);

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, Selector7BaseTestAndSkips) {
    Simple8b s8b;
    // 57344 = 1110000000000000 = 3 value bits and 13 zeros
    // This should be encoded with alternating skips as:
    // [0010] [(111) (1111) (111) (1101)] x 4 [0111] = 602C00B0607D81F7
    uint64_t val = 57344;
    std::vector<Simple8b::Value> expectedValues;
    for (uint32_t i = 0; i < 5; i++) {
        ASSERT_TRUE(s8b.append(val));
        s8b.skip();
        expectedValues.push_back({i * 2, val});
    }
    // test that buffer was correct
    std::vector<uint8_t> expectedChar = {0xD7, 0xFF, 0xF7, 0xFF, 0xFD, 0x7F, 0xFF, 0x2F};
    testUnderlyingBufferWithoutFlush(s8b, expectedChar);

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, Selector7SingleZero) {
    // 30 = 11110 so a single zero
    // This should not be encoded with selector 7  since it will take 4 extra bits to store the
    // count of zeros
    // This should be encoded as:
    // [0000] [11110] x 12 [0101] = F7BDEF7BDEF7BDE5
    Simple8b s8b;
    uint64_t val = 30;
    std::vector<Simple8b::Value> expectedValues;
    for (uint32_t i = 0; i < 13; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i, val});
    }
    // test that buffer was correct
    std::vector<uint8_t> expectedChar = {0xE5, 0xBD, 0xF7, 0xDE, 0x7B, 0xEF, 0xBD, 0xF7};
    testUnderlyingBufferWithoutFlush(s8b, expectedChar);

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(expectedValues, values);
}

TEST(Simple8b, Selector7SkipEncoding) {
    Simple8b s8b;
    // 229376 = 111000000000000000 = 3 value bits and 15 zeros which would be stored as 111-1111
    // using selector 7. However, we will add a padding bit to store as 0111-1111
    // This should be encoded as:
    // [0011] [(0111) (1111)] x7 [0111] = 37F7F7F7F7F7F7F7
    uint64_t val = 229376;
    std::vector<Simple8b::Value> expectedValues;
    for (uint32_t i = 0; i < 10; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i, val});
    }
    // test that buffer was correct
    std::vector<uint8_t> expectedChar = {0xF7, 0xF7, 0xF7, 0xF7, 0xF7, 0xF7, 0xF7, 0x37};
    testUnderlyingBufferWithoutFlush(s8b, expectedChar);

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simpl8b, Selector7IntegrationWithBaseEncoding) {
    Simple8b s8b;
    // Base value = 1011 = 11
    // Selector 7 value = 12615680 = 110000000001 + 15 zeros.
    // We should encode this as:
    // [0110] [(01100000001) (1111)] x 2 [(00000001011) (0000)] x 2 [0111] = 6607D81F02C00B07
    uint64_t val = 11;
    std::vector<Simple8b::Value> expectedValues;
    for (uint32_t i = 0; i < 2; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i, val});
    }

    val = 12615680;
    for (uint32_t i = 0; i < 3; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i + 2, val});
    }

    // test that buffer was correct
    std::vector<uint8_t> expectedChar = {0x07, 0x0B, 0xC0, 0x02, 0x1F, 0xD8, 0x07, 0x66};
    testUnderlyingBufferWithoutFlush(s8b, expectedChar);

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simpl8b, Selector7IntegrationWithBaseEncodingOppositeDirection) {
    Simple8b s8b;
    // Base value = 1011 = 11
    // Selector 7 value = 12615680 = 110000000001 + 15 zeros.
    // We should encode this as:
    // [0110] [(00000001011) (0000)] x 2 [(01100000001) (1111)] x 2 [0111] = 602C00B0607D81F7
    uint64_t val = 12615680;
    std::vector<Simple8b::Value> expectedValues;
    for (uint32_t i = 0; i < 2; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i, val});
    }

    val = 11;
    for (uint32_t i = 0; i < 3; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i + 2, val});
    }

    // test that buffer was correct
    std::vector<uint8_t> expectedChar = {0xF7, 0x81, 0x7D, 0x060, 0xB0, 0x00, 0x2C, 0x60};
    testUnderlyingBufferWithoutFlush(s8b, expectedChar);

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, Selector8SmallBaseTest) {
    Simple8b s8b;
    // 458752 = 111 + 16 zeros. This should be stored as (0111 0100) where the second value of 4 is
    // the nibble shift of 4*4. The first value is 0111 because we  store at least 4 values. 0001
    // This should be encoded as
    // [0001] [(0111) (0100)] x 7 [1000] = 1747474747474748
    uint64_t val = 458752;
    std::vector<Simple8b::Value> expectedValues;
    for (uint32_t i = 0; i < 9; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i, val});
    }
    // test that buffer was correct
    std::vector<uint8_t> expectedChar = {0x48, 0x47, 0x47, 0x47, 0x47, 0x47, 0x47, 0x17};
    testUnderlyingBufferWithoutFlush(s8b, expectedChar);

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, Selector8SmallBaseTestAndSkip) {
    Simple8b s8b;
    // 7340032 = 111 + 20 zeros. This should be stored as (0111 0101) where the second value of 4 is
    // the nibble shift of 4*4. The first value is 0111 because we  store at least 4 values. Then we
    // have a value of all 1s for skip.
    // This should be encoded as
    // [0001] [(0111) (0101)] [(1111 1111) (0111 0101)] x 3 [1000] = 175FF75FF75FF758
    uint64_t val = 7340032;
    std::vector<Simple8b::Value> expectedValues;
    for (uint32_t i = 0; i < 4; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i * 2, val});
        s8b.skip();
    }
    // test that buffer was correct
    std::vector<uint8_t> expectedChar = {0x58, 0xF7, 0x5F, 0xF7, 0x5F, 0xF7, 0x5F, 0x17};
    testUnderlyingBufferWithoutFlush(s8b, expectedChar);

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, Selector8SmallSkipEncoding) {
    Simple8b s8b;
    // A perfect skip value is one that aligns perfectly with the boundary. 1111 with 60 zeros does
    // that.
    // This should be encoded as
    // [0010] [(01111 1111) x 6] [1000] = 23FEFFFFBFFFEFF8
    uint64_t val = 17293822569102704640ull;
    std::vector<Simple8b::Value> expectedValues;
    for (uint32_t i = 0; i < 5; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i * 2, val});
        s8b.skip();
    }
    // test that buffer was correct
    std::vector<uint8_t> expectedChar = {0xF8, 0xEF, 0xFF, 0xBF, 0xFF, 0xFF, 0xFE, 0x23};
    testUnderlyingBufferWithoutFlush(s8b, expectedChar);

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, Selector8SmallNibbleShift) {
    Simple8b s8b;
    // 7864320 = 1111 + 19 zeros. This is a value that should have 3 trailing zeros due to nibble.
    // So we should encode as:
    // [0011] [(1111000) (0100)] x 4 [1000] = 3784F09E13C27848
    uint64_t val = 7864320;
    std::vector<Simple8b::Value> expectedValues;
    for (uint32_t i = 0; i < 6; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i, val});
    }
    // test that buffer was correct
    std::vector<uint8_t> expectedChar = {0x48, 0x78, 0xC2, 0x13, 0x9E, 0xF0, 0x84, 0x37};
    testUnderlyingBufferWithoutFlush(s8b, expectedChar);

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, Selector8SmallBitsAndNibble) {
    Simple8b s8b;
    // 549789368320 = 1(13 zeros)1 + 25 zeros. This is a value that should have 1 trailing zeros due
    // to nibble. So we should encode as:
    // [0110] [(0000001(13 zeros)10) (0110)] x2 [1000] = 6008002600800268
    uint64_t val = 549789368320;
    std::vector<Simple8b::Value> expectedValues;
    for (uint32_t i = 0; i < 3; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i, val});
    }
    // test that buffer was correct
    std::vector<uint8_t> expectedChar = {0x68, 0x02, 0x80, 0x00, 0x26, 0x00, 0x08, 0x60};
    testUnderlyingBufferWithoutFlush(s8b, expectedChar);

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, Selector8SmallAddSelector7FirstThen8) {
    Simple8b s8b;
    // This tests that what only requires a seven selector value will be properly encoded as a eight
    // selector value. The 57344 should be encoded as 3 ones and 13 zeros. The next value requires
    // selector 8 which is 7340032 (3 ones and 20 zeros). We should choose a selector requiring 4
    // valu bits to store these.
    // This should be encoded as
    // [0001] [(0111) (0101) x 3] [(1110 0011) x 4] [1000] = 1757575E3E3E3E38
    uint64_t val = 57344;
    std::vector<Simple8b::Value> expectedValues;
    for (uint32_t i = 0; i < 4; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i, val});
    }

    val = 7340032;
    for (uint32_t i = 0; i < 4; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i + 4, val});
    }

    // test that buffer was correct
    std::vector<uint8_t> expectedChar = {0x38, 0x3E, 0x3E, 0x3E, 0x5E, 0x57, 0x57, 0x17};
    testUnderlyingBufferWithoutFlush(s8b, expectedChar);

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, Selector8SmallAddBaseSelectorThen7Then8) {
    Simple8b s8b;
    // This tests a combination of selector switches, rounding, and nibble shifts for our simple8b
    // encoding. The first value of 6 is 110 which should be encoded as 0110 0000. The 57344 should
    // be encoded as 3 ones and 13 zeros. The next value requires selector 8 which is 7340032 (3
    // ones and 20 zeros). We should choose a selector requiring 4 valu bits to store these. This
    // sohould be encoded as: [0001] [(0111) (0101) x 3] [(1110 0011) x 3] [0110 0000] [1000] =
    // 17575E3E3E3E3608
    uint64_t val = 6;
    ASSERT_TRUE(s8b.append(val));
    std::vector<Simple8b::Value> expectedValues;
    expectedValues.push_back({0, val});

    val = 57344;
    for (uint32_t i = 0; i < 4; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i + 1, val});
    }

    val = 7340032;
    for (uint32_t i = 0; i < 4; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i + 5, val});
    }

    // test that buffer was correct 17474E3E3E3E3608
    std::vector<uint8_t> expectedChar = {0x08, 0x36, 0x3E, 0x3E, 0x3E, 0x5E, 0x57, 0x17};
    testUnderlyingBufferWithoutFlush(s8b, expectedChar);

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, Selector8SmallStartWith8SelectorAndAddSmallerValues) {
    Simple8b s8b;
    // This tests a combination of selector switches, rounding, and nibble shifts for our simple8b
    // encoding. TThe first value requires selector 8 which is 7340032 (3 ones
    // and 20 zeros).  The 57344 should
    // be encoded as 3 ones and 13 zeros. The next value of 6 is 110 which should be encoded as 0110
    // 0000. We should choose a selector requiring 4 value bits to store these. This should be
    // encoded as [0001] [0110 0000] [(1110 0011) x 3] [(0111) (0101) x 3] = 160E3E3E37575758
    std::vector<Simple8b::Value> expectedValues;
    uint64_t val = 7340032;
    for (uint32_t i = 0; i < 3; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i, val});
    }

    val = 57344;
    for (uint32_t i = 0; i < 3; i++) {
        ASSERT_TRUE(s8b.append(val));
        expectedValues.push_back({i + 3, val});
    }

    val = 6;
    ASSERT_TRUE(s8b.append(val));
    expectedValues.push_back({6, val});

    ASSERT_TRUE(s8b.append(val));
    expectedValues.push_back({7, val});

    // test that buffer was correct
    std::vector<uint8_t> expectedChar = {0x58, 0x57, 0x57, 0x37, 0x3E, 0x3E, 0x0E, 0x16};
    testUnderlyingBufferWithoutFlush(s8b, expectedChar);

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);
}

TEST(Simple8b, Rle) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(180, 1);
    testAppendAndGetAllInts(s8b, expectedInts);

    std::vector<uint8_t> expectedChar{
        // The selector is 2 and there are 30 bucket with the same value 0b01.
        0x52,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,  // 1st word.
        // The selector is 15 and the word is RLE encoding with count = 1.
        0x0F,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,  // 2nd word.
        // The selector is 2 and there are 30 bucket with the same value 0b01.
        0x52,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,  // 3rd word.
    };
    testUnderlyingBufferWithFlush(s8b, expectedChar);
}

TEST(Simple8b, RleFlush) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(121, 1);
    std::vector<Simple8b::Value> expectedValues{};

    ASSERT_TRUE(s8b.append(expectedInts[0]));
    expectedValues.push_back({0, expectedInts[0]});
    s8b.flush();

    for (size_t i = 1; i < expectedInts.size(); ++i) {
        expectedValues.push_back({(uint32_t)i, expectedInts[i]});
        ASSERT_TRUE(s8b.append(expectedInts[i]));
    }

    std::vector<Simple8b::Value> values = s8b.getAllInts();
    assertVectorsEqual(values, expectedValues);

    std::vector<uint8_t> expectedChar{
        // The selector is 15 and there is only 1 bucket with the value 1.
        0x1E,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,  // 1st word.
        // The selector is 15 and the word is RLE encoding with count = 1.
        0x0F,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,  // 2nd word.
    };
    testUnderlyingBufferWithFlush(s8b, expectedChar);
}

TEST(Simple8b, MultipleRleWords) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(30 + (16 * 120 * 2), 1);
    testAppendAndGetAllInts(s8b, expectedInts);

    std::vector<uint8_t> expectedChar{
        // The selector is 2 and there are 30 bucket with the same value 0b01.
        0x52,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,  // 1st word.
        // The selector is 15 and the word is RLE with max count = 16.
        0xFF,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,  // 2nd word.
        // The selector is 15 and the word is RLE with max count = 16.
        0xFF,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,  // 3rd word.
    };
    testUnderlyingBufferWithFlush(s8b, expectedChar);
}

TEST(Simple8b, RleSkip) {
    Simple8b s8b;

    int numSkips = 240;
    std::vector<uint64_t> expectedInts{};
    for (int i = 0; i < numSkips; ++i)
        s8b.skip();

    // testAppendAndGetAllInts(s8b, expectedInts);

    std::vector<uint8_t> expectedChar{
        // The selector is 1 and there are 60 bucket with skip.
        0xF1,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,  // 1st word.
        // The selector is 15 and the word is RLE encoding with count = 1.
        0x0F,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,  // 2nd word.
        // The selector is 1 and there are 60 bucket with skip.
        0xF1,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,  // 3rd word.
    };
    testUnderlyingBufferWithFlush(s8b, expectedChar);
}

TEST(Simple8b, RleChangeOfValue) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(300, 1);
    expectedInts.push_back(7);
    testAppendAndGetAllInts(s8b, expectedInts);

    std::vector<uint8_t> expectedChar{
        // The selector is 2 and there are 30 bucket with the same value 0b01.
        0x52,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,  // 1st word.
        // The selector is 15 and the word is RLE encoding with count = 2.
        0x1F,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,  // 2nd word.
        // The selector is 2 and there are 30 bucket with the same value 0b01.
        0x52,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,  // 3rd word.
        // The selector is 14 and there is only one bucket with the value 7.
        0x7E,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,  // 4th word.
    };
    testUnderlyingBufferWithFlush(s8b, expectedChar);
}

TEST(Simple8b, RleDefaultValue) {
    Simple8b s8b;

    std::vector<uint64_t> expectedInts(240, 0);
    testAppendAndGetAllInts(s8b, expectedInts);

    std::vector<uint8_t> expectedChar{
        // The selector is 15 and the word is RLE encoding with count = 2.
        0x1F,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,  // 1st word.
    };
    testUnderlyingBufferWithFlush(s8b, expectedChar);
}
