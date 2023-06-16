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

#include "mongo/bson/util/simple8b.h"

#include <absl/numeric/int128.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/simple8b_builder.h"
#include "mongo/platform/int128.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/shared_buffer.h"

using namespace mongo;

template <typename T>
void assertValuesEqual(const Simple8b<T>& actual, const std::vector<boost::optional<T>>& expected) {
    auto it = actual.begin();
    auto end = actual.end();
    ASSERT_TRUE(it.valid());

    size_t i = 0;
    for (; i < expected.size() && it != end; ++i, ++it) {
        ASSERT_EQ(*it, expected[i]);
        ASSERT_TRUE(it.more());
    }

    ASSERT(it == end);
    ASSERT_EQ(i, expected.size());
    ASSERT_TRUE(it.valid());
    ASSERT_FALSE(it.more());
}

template <typename T>
std::pair<SharedBuffer, int> buildSimple8b(const std::vector<boost::optional<T>>& expectedValues) {
    BufBuilder _buffer;
    Simple8bBuilder<T> builder([&_buffer](uint64_t simple8bBlock) {
        _buffer.appendNum(simple8bBlock);
        return true;
    });
    for (const auto& elem : expectedValues) {
        if (elem) {
            ASSERT_TRUE(builder.append(*elem));
        } else {
            builder.skip();
        }
    }
    builder.flush();

    auto size = _buffer.len();
    return {_buffer.release(), size};
}

template <typename T>
void testSimple8b(const std::vector<boost::optional<T>>& expectedValues,
                  const std::vector<uint8_t>& expectedBinary) {
    auto [buffer, size] = buildSimple8b(expectedValues);

    ASSERT_EQ(size, expectedBinary.size());
    if (size > 0) {
        ASSERT_EQ(memcmp(buffer.get(), expectedBinary.data(), size), 0);
    }

    Simple8b<T> s8b(buffer.get(), size);
    assertValuesEqual(s8b, expectedValues);
}

template <typename T>
void testSimple8b(const std::vector<boost::optional<T>>& expectedValues) {
    auto [buffer, size] = buildSimple8b<T>(expectedValues);

    Simple8b<T> s8b(buffer.get(), size);
    assertValuesEqual(s8b, expectedValues);
}

TEST(Simple8b, NoValues) {
    std::vector<boost::optional<uint64_t>> expectedInts = {};
    std::vector<uint8_t> expectedBinary = {};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Null) {
    Simple8b<uint64_t> s8b;
    ASSERT_FALSE(s8b.begin().valid());
    ASSERT_FALSE(s8b.begin().more());
    ASSERT(s8b.begin() == s8b.end());
}

TEST(Simple8b, OnlySkip) {
    std::vector<boost::optional<uint64_t>> expectedInts = {boost::none};

    // The selector is 14 and the remaining 60 bits of data are all 1s, which represents skip.
    std::vector<uint8_t> expectedBinary{
        0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // 1st word.

    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, OneValue) {
    std::vector<boost::optional<uint64_t>> expectedInts = {1};
    // The selector is 14 and there is only 1 bucket with the value 1.
    std::vector<uint8_t> expectedBinary{0x1E, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};  // 1st word.

    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, MaxValue) {
    std::vector<boost::optional<uint64_t>> expectedInts = {0xFFFFFFFFFFFFFFE};

    // The selector is 14 and there is only 1 bucket with the max possible value 0xFFFFFFFFFFFFFFE.
    std::vector<uint8_t> expectedBinary{
        0xEE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // 1st word.

    testSimple8b(expectedInts, expectedBinary);
}
TEST(Simple8b, MultipleValues) {
    std::vector<boost::optional<uint64_t>> expectedInts = {1, 2, 3};

    // The selector is 12 and there are 3 bucket with the values 1, 2 and 3.
    std::vector<uint8_t> expectedBinary{0x1C, 0x0, 0x0, 0x2, 0x0, 0x30, 0x0, 0x0};  // 1st word.

    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, MaxValues) {
    std::vector<boost::optional<uint64_t>> expectedInts(60, 1);

    // The selector is 2 and there are 30 bucket with the same value 0b01. 0x55 = 0b01010101.
    std::vector<uint8_t> expectedBinary{
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

    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, EncodeWithTrailingDirtyBits) {
    std::vector<boost::optional<uint64_t>> expectedInts(7, 1);

    // The selector is 8 and there are 7 bucket with the same value 0b00000001.
    // The last 4 bits are dirty/unused.
    std::vector<uint8_t> expectedBinary{
        0x08, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};  // 1st word.

    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, FullBuffer) {
    std::vector<boost::optional<uint64_t>> expectedInts(120, 1);

    // The selector is 2 and there are 30 bucket with the same value 0b01. 0x55 = 0b01010101.
    std::vector<uint8_t> expectedBinary{
        0x52, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,  // 1st word.
        0x52, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,  // 2nd word.
        0x52, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,  // 3rd word.
        0x52, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55   // 4th word.
    };

    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, MaxValueBuffer) {
    std::vector<boost::optional<uint64_t>> expectedInts(3, 0xFFFFFFFFFFFFFFE);

    // The selector is 14 and there is only 1 bucket with the max possible value 0xFFFFFFFFFFFFFFE.
    std::vector<uint8_t> expectedBinary{
        0xEE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 1st word.
        0xEE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 2nd word.
        0xEE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF   // 3rd word.
    };

    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, TrySomeSmallValues) {
    std::vector<boost::optional<uint64_t>> expectedInts;
    for (size_t num = 0; num <= 0x1FFFF; ++num) {
        expectedInts.push_back(num);
    }

    testSimple8b(expectedInts);
}

TEST(Simple8b, TrySomeLargeValues) {
    std::vector<boost::optional<uint64_t>> expectedInts;
    for (size_t num = 0xF00000000; num <= 0xF0001FFFF; ++num) {
        expectedInts.push_back(num);
    }

    testSimple8b(expectedInts);
}

TEST(Simple8b, BreakPendingIntoMultipleSimple8bBlocks) {
    std::vector<boost::optional<uint64_t>> expectedInts(57, 1);

    // 15 is 0b1111 and can not be added to the current word because it would overflow.
    // We can not form a 57 bit word because we would be unable to determine
    // if the last 3 bits are empty or unused.
    // Therefore, we must form a word with 30 integers of 1's, 20 integers of 1's
    // and the current vector would have seven 1's and one 15.
    expectedInts.push_back(15);

    std::vector<uint8_t> expectedBinary{
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
        0x07,
        0x81,
        0x40,
        0x20,
        0x10,
        0x08,
        0x04,
        0x1E  // 3rd word.
    };

    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, BreakPendingValuesIntoMultipleSimple8bWords) {
    std::vector<boost::optional<uint64_t>> expectedInts(50, 0);

    // 0xFFFFFFFFFFFF is 48 bits and can not be added to the current word because it would overflow.
    // We can not form a 57 bit word because we would be unable to determine
    // if the last 3 bits are empty or unused. Therefore, we must form a word with 30 integers
    // of 0's and 20 integers of 0's in the same append() iteration.
    expectedInts.push_back(0xFFFFFFFFFFFF);  // 48 bit value.

    std::vector<uint8_t> expectedBinary{
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

    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, SkipAtSecondToLast) {
    std::vector<boost::optional<uint64_t>> expectedInts(3, 3);
    expectedInts.push_back(boost::none);
    expectedInts.push_back(7);

    // The selector is 10 and there are 5 bucket with 12 bit buckets.
    // 0xFF and 0x7F is 15 1's. The skip is 12 1's and 7 is 3 1's.
    std::vector<uint8_t> expectedBinary{0x3A, 0x0, 0x3, 0x30, 0x0, 0xFF, 0x7F, 0x0};  // 1st word.

    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, SkipInMiddle) {
    std::vector<boost::optional<uint64_t>> expectedInts(50, 1);
    expectedInts.push_back(boost::none);
    expectedInts.insert(expectedInts.end(), 50, 1);

    std::vector<uint8_t> expectedBinary{
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
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, TrailingSkips) {
    std::vector<boost::optional<uint64_t>> expectedInts(48, 1);
    expectedInts.insert(expectedInts.end(), 2, boost::none);

    std::vector<uint8_t> expectedBinary{
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
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, LeadingSkips) {
    std::vector<boost::optional<uint64_t>> expectedInts = {3, 8, 13};
    expectedInts.insert(expectedInts.begin(), 2, boost::none);

    // The selector is 10 and there are 5 bucket with 12 bit buckets.
    // The first two buckets are skips.
    std::vector<uint8_t> expectedBinary{0xFA, 0xFF, 0xFF, 0x3F, 0x0, 0x8, 0xD0, 0x0};  // 1st word.

    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, WordOfSkips) {
    std::vector<boost::optional<uint64_t>> expectedInts(30, boost::none);
    uint64_t numWithMoreThanThirtyBits = (1ull << 30) + 1;
    expectedInts.push_back(numWithMoreThanThirtyBits);

    std::vector<uint8_t> expectedBinary{
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

    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, LargeSkipsFirst) {
    std::vector<boost::optional<uint64_t>> expectedInts;

    for (uint32_t i = 0; i < 10; ++i) {
        expectedInts.push_back(boost::none);
        expectedInts.push_back(64);
    }

    std::vector<uint8_t> expectedChar{
        // The selector is 7 and the extension value is 1, so the values alternate
        // between 0b111111 (skip) and 0b010110 (64).
        0x17,
        0xBF,
        0xF5,
        0x5B,
        0xBF,
        0xF5,
        0x5B,
        0x3F,  // 1st word.
        // The selector is 7 and the extension value is 1, so the values alternate
        // between 0b111111 (skip) and 0b010110 (64).
        0x17,
        0xD6,
        0x6F,
        0xFD,
        0xD6,
        0x6F,
        0xFD,
        0x16,  // 2nd word.
        // The selector is 13 and there are 2 buckets with one skip and then 0b010000 (64).
        0xFD,
        0xFF,
        0xFF,
        0xFF,
        0x03,
        0x01,
        0x0,
        0x0,  // 3rd word.
    };

    testSimple8b(expectedInts, expectedChar);
}

TEST(Simple8b, RleZeroThenRleAnotherValue) {
    std::vector<boost::optional<uint64_t>> expectedInts(1920, 0);

    expectedInts.insert(expectedInts.end(), 270, 1);

    std::vector<uint8_t> expectedChar{
        // The selector is 15 and the word is a RLE encoding with count = 16.
        // The default RLE value is 0 if it is the first number.
        0xFF,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,  // 1st word.
        // The selector is 2 and there are 30 bucket with the same value 0b01. 0x55 = 0b01010101.
        0x52,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,
        0x55,  // 2nd word.
        // The selector is 15 and the word is a RLE encoding with count = 2.
        0x1F,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,
        0x0,  // 3rd word.
    };

    testSimple8b(expectedInts, expectedChar);
}

TEST(Simple8b, MultipleFlushes) {
    BufBuilder buffer;
    Simple8bBuilder<uint64_t> s8b([&buffer](uint64_t simple8bBlock) {
        buffer.appendNum(simple8bBlock);
        return true;
    });

    std::vector<uint64_t> values = {1};
    for (size_t i = 0; i < values.size(); ++i) {
        ASSERT_TRUE(s8b.append(values[i]));
    }

    s8b.flush();

    values[0] = 2;
    for (size_t i = 0; i < values.size(); ++i) {
        ASSERT_TRUE(s8b.append(values[i]));
    }

    std::vector<uint8_t> expectedBinary{
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

    s8b.flush();

    char* hex = buffer.buf();
    size_t len = buffer.len();
    ASSERT_EQ(len, expectedBinary.size());

    for (size_t i = 0; i < len; ++i) {
        ASSERT_EQ(static_cast<uint8_t>(*hex), expectedBinary[i]) << i;
        ++hex;
    }
}

TEST(Simple8b, Selector7BaseTest) {
    // 57344 = 1110000000000000 = 3 value bits and 13 zeros
    // This should be encoded as:
    // [(111) (1101)] x 8 [0010] [0111] = FBF7EFDFBF7EFD27
    // This is in hex: 2FBF7EFDFBF7EFD7
    uint64_t val = 57344;

    std::vector<boost::optional<uint64_t>> expectedInts(8, val);

    // test that buffer was correct
    std::vector<uint8_t> expectedBinary = {0x27, 0xFD, 0x7E, 0xBF, 0xDF, 0xEF, 0xF7, 0xFB};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Selector7BaseTestAndSkips) {
    // 57344 = 1110000000000000 = 3 value bits and 13 zeros
    // This should be encoded with alternating skips as:
    // [(111) (1111) (111) (1101)] x 4 [0010] [0111] = FFF7FFDFFF7FFD27
    uint64_t val = 57344;
    std::vector<boost::optional<uint64_t>> expectedInts;

    for (uint32_t i = 0; i < 4; i++) {
        expectedInts.push_back(val);
        expectedInts.push_back(boost::none);
    }

    // test that buffer was correct
    std::vector<uint8_t> expectedBinary = {0x27, 0xFD, 0x7F, 0xFF, 0xDF, 0xFF, 0xF7, 0xFF};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Selector7SingleZero) {
    // 30 = 11110 so a single zero
    // This should not be encoded with selector 7  since it will take 4 extra bits to store the
    // count of zeros
    // This should be encoded as:
    // [11110] x 12 [0101] = F7BDEF7BDEF7BDE5
    uint64_t val = 30;
    std::vector<boost::optional<uint64_t>> expectedInts(12, val);

    // test that buffer was correct
    std::vector<uint8_t> expectedBinary = {0xE5, 0xBD, 0xF7, 0xDE, 0x7B, 0xEF, 0xBD, 0xF7};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Selector7SkipEncoding) {
    // 229376 = 111000000000000000 = 3 value bits and 15 zeros which would be stored as 111-1111
    // using selector 7. However, we will add a padding bit to store as 0111-1111
    // This should be encoded as:
    // [(0111) (1111)] x7 [0011] [0111] = 7F7F7F7F7F7F7F37
    uint64_t val = 229376;
    std::vector<boost::optional<uint64_t>> expectedInts(7, val);

    // test that buffer was correct
    std::vector<uint8_t> expectedBinary = {0x37, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simpl8b, Selector7IntegrationWithBaseEncoding) {
    // Base value = 1011 = 11
    // Selector 7 value = 12615680 = 110000000001 + 15 zeros.
    // We should encode this as:
    // [(01100000001) (1111)] x 2 [(00000001011) (0000)] x 2 [0110] [0111] = 607
    // D81F02C00B067
    uint64_t val = 11;
    std::vector<boost::optional<uint64_t>> expectedInts(2, val);

    val = 12615680;
    expectedInts.insert(expectedInts.end(), 2, val);

    // test that buffer was correct
    std::vector<uint8_t> expectedBinary = {0x67, 0xB0, 0x00, 0x2C, 0xF0, 0x81, 0x7D, 0x60};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simpl8b, Selector7IntegrationWithBaseEncodingOppositeDirection) {
    // Base value = 1011 = 11
    // Selector 7 value = 12615680 = 110000000001 + 15 zeros.
    // We should encode this as:
    // [(00000001011) (0000)] x 2 [(01100000001) (1111)] x 2 [0110] [0111] = 2C0
    // 0B0607D81F67

    uint64_t val = 12615680;
    std::vector<boost::optional<uint64_t>> expectedInts(2, val);

    val = 11;
    expectedInts.insert(expectedInts.end(), 2, val);

    // test that buffer was correct
    std::vector<uint8_t> expectedBinary = {0x67, 0x1F, 0xD8, 0x07, 0x06, 0x0B, 0xC0, 0x02};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Selector8SmallBaseTest) {
    // 0x500000 = 101 + 20 zeros. This should be stored as (0101 0101) where the second value of 4
    // is the nibble shift of 5*4. The first value is 0101 because we store at least 4 bits. This
    // should be encoded as
    // [(0101) (0101)] x 7 [0001] [1000] = 7575757575757518
    //
    uint64_t val = 0x500000;
    std::vector<boost::optional<uint64_t>> expectedInts(7, val);

    // test that buffer was correct
    std::vector<uint8_t> expectedBinary = {0x18, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Selector8SmallBaseTestAndSkip) {
    // 7340032 = 111 + 20 zeros. This should be stored as (0111 0101) where the second value of 4 is
    // the nibble shift of 4*4. The first value is 0111 because we  store at least 4 values. Then we
    // have a value of all 1s for skip.
    // This should be encoded as
    // [(0111) (0101)] [(1111 1111) (0111 0101)] x 3 [0001] [1000] = 75FF75FF75FF7518
    uint64_t val = 7340032;
    std::vector<boost::optional<uint64_t>> expectedInts;
    expectedInts.push_back(val);
    for (uint32_t i = 0; i < 3; i++) {
        expectedInts.push_back(boost::none);
        expectedInts.push_back(val);
    }

    // test that buffer was correct
    std::vector<uint8_t> expectedBinary = {0x18, 0x75, 0xFF, 0x75, 0xFF, 0x75, 0xFF, 0x75};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Selector8SmallSkipEncoding) {
    // A perfect skip value is one that aligns perfectly with the boundary. 1111 with 60 zeros does
    // that. They need to be padded with an extra zero and cause the Selector to be 2 instead of 1
    // bumping out the last skip to the next block.
    // This should be encoded as
    // [(01111 1111) x 6] [0010] [1000] = 3FEFFFFBFFFEFF28
    //
    uint64_t val = 17293822569102704640ull;
    std::vector<boost::optional<uint64_t>> expectedInts;
    for (uint32_t i = 0; i < 3; i++) {
        expectedInts.push_back(val);
        expectedInts.push_back(boost::none);
    }

    // test that buffer was correct
    std::vector<uint8_t> expectedBinary = {0x28, 0xFF, 0xFE, 0xFF, 0xFB, 0xFF, 0xEF, 0x3F};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Selector8SmallNibbleShift) {
    // 7864320 = 1111 + 19 zeros. This is a value that should have 3 trailing zeros due to nibble.
    // So we should encode as:
    // [(1111000) (0100)] x 4 [0011] [1000] = 784F09E13C278438
    uint64_t val = 7864320;
    std::vector<boost::optional<uint64_t>> expectedInts(5, val);

    // test that buffer was correct
    std::vector<uint8_t> expectedBinary = {0x38, 0x84, 0x27, 0x3C, 0xE1, 0x09, 0x4F, 0x78};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Selector8SmallBitsAndNibble) {
    // 549789368320 = 1(13 zeros)1 + 25 zeros. This is a value that should have 1 trailing zeros due
    // to nibble. So we should encode as:
    // [(0000001(13 zeros)10) (0110)] x2 [0110] [1000] = 80026008002668
    uint64_t val = 549789368320;
    std::vector<boost::optional<uint64_t>> expectedInts(2, val);

    // test that buffer was correct
    std::vector<uint8_t> expectedBinary = {0x68, 0x26, 0x00, 0x08, 0x60, 0x02, 0x80, 0x00};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Selector8SmallAddSelector7FirstThen8) {
    // This tests that what only requires a seven selector value will be properly encoded as a eight
    // selector value. The 57344 should be encoded as 3 ones and 13 zeros. The next value requires
    // selector 8 which is 7340032 (3 ones and 20 zeros). We should choose a selector requiring 4
    // valu bits to store these.
    // This should be encoded as
    // [(0111) (0101) x 3] [(1110 0011) x 4] [0001] [1000] = 757575E3E3E3E318
    uint64_t val = 57344;
    std::vector<boost::optional<uint64_t>> expectedInts(4, val);

    val = 7340032;
    expectedInts.insert(expectedInts.end(), 3, val);

    // test that buffer was correct
    std::vector<uint8_t> expectedBinary = {0x18, 0xE3, 0xE3, 0xE3, 0xE3, 0x75, 0x75, 0x75};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Selector8SmallAddBaseSelectorThen7Then8) {
    // This tests a combination of selector switches, rounding, and nibble shifts for our simple8b
    // encoding. The first value of 6 is 110 which should be encoded as 0110 0000. The 57344 should
    // be encoded as 3 ones and 13 zeros. The next value requires selector 8 which is 7340032 (3
    // ones and 20 zeros). We should choose a selector requiring 4 valu bits to store these. This
    // sohould be encoded as: [(0111) (0101) x 3] [(1110 0011) x 3] [0110 0000] [0001] [1000] =
    // 17575E3E3E3E3608
    uint64_t val = 6;
    std::vector<boost::optional<uint64_t>> expectedInts(1, val);

    val = 57344;
    expectedInts.insert(expectedInts.end(), 4, val);

    val = 7340032;
    expectedInts.insert(expectedInts.end(), 2, val);

    // test that buffer was correct 17474E3E3E3E3608
    std::vector<uint8_t> expectedBinary = {0x18, 0x60, 0xE3, 0xE3, 0xE3, 0xE3, 0x75, 0x75};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Selector8SmallStartWith8SelectorAndAddSmallerValues) {
    // This tests a combination of selector switches, rounding, and nibble shifts for our simple8b
    // encoding. TThe first value requires selector 8 which is 7340032 (3 ones
    // and 20 zeros).  The 57344 should
    // be encoded as 3 ones and 13 zeros. The next value of 6 is 110 which should be encoded as 0110
    // 0000. We should choose a selector requiring 4 value bits to store these. This should be
    // encoded as
    // [0110 0000] [(1110 0011) x 3] [(0111) (0101) x 3] [0001] [1000] = 60E3E3E375757518

    uint64_t val = 7340032;
    std::vector<boost::optional<uint64_t>> expectedInts(3, val);

    val = 57344;
    expectedInts.insert(expectedInts.end(), 3, val);

    val = 6;
    expectedInts.insert(expectedInts.end(), 1, val);

    // test that buffer was correct
    std::vector<uint8_t> expectedBinary = {0x18, 0x75, 0x75, 0x75, 0xE3, 0xE3, 0xE3, 0x60};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Rle) {
    std::vector<boost::optional<uint64_t>> expectedInts(180, 1);

    std::vector<uint8_t> expectedBinary{
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
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, MultipleRleWords) {
    std::vector<boost::optional<uint64_t>> expectedInts(30 + (16 * 120 * 2), 1);

    std::vector<uint8_t> expectedBinary{
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
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, RleSkip) {
    std::vector<boost::optional<uint64_t>> expectedInts(240, boost::none);

    std::vector<uint8_t> expectedBinary{
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
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, FlushSkipRLE) {
    // Make sure that flushing skips does not re-enable RLE when it fits a full Simple8b. We need at
    // least 121 skips to verify this (60+60+1)
    std::vector<boost::optional<uint64_t>> expectedInts(121, boost::none);

    std::vector<uint8_t> expectedBinary{
        // The selector is 1 and there are 60 bucket with skip.
        0xF1,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,  // 1st word.
        // The selector is 1 and there are 60 bucket with skip.
        0xF1,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,  // 2nd word.
        // The selector is 14 and there are 1 bucket with skip.
        0xFE,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,  // 3rd word.
    };

    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, RleChangeOfValue) {
    std::vector<boost::optional<uint64_t>> expectedInts(300, 1);
    expectedInts.push_back(7);

    std::vector<uint8_t> expectedBinary{
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

    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, RleFront) {
    std::vector<boost::optional<uint64_t>> expectedInts(240, 0);

    std::vector<uint8_t> expectedBinary{
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

    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, EightSelectorLargeBase) {
    // 8462480737302404222943232 = 111 + 80 zeros. This should be stored as (111 10100) where the
    // second value of 20 is the nibble shift of 4*20. The first value is 0111 because we store at
    // least 4 values. This should be encoded as [(111) (10100)] x6 [1000] [1000] = //
    // 81E8F47A3D1E8F48
    uint128_t val = absl::MakeUint128(0x70000, 0x0);
    std::vector<boost::optional<uint128_t>> expectedInts = {val, val, val, val, val, val};
    std::vector<uint8_t> expectedBinary = {0x88, 0xF4, 0xE8, 0xD1, 0xA3, 0x47, 0x8F, 0x1E};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, UInt128Zero) {
    // Have a large value that forces the extended selectors to be used. Then we check that zeros
    // are handled correctly for them.
    uint128_t val =
        absl::MakeUint128(0x70000, 0x0);  // Stored as 0xF4, [value=(111) nibble count=(10100)]
    uint128_t zero = absl::MakeUint128(0x0, 0x0);

    // 5 values with Selector8Large = 0x98
    std::vector<boost::optional<uint128_t>> expectedInts = {val, zero, zero, zero, zero};
    std::vector<uint8_t> expectedBinary = {0x98, 0xF4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Selector8LargeBaseTestAndSkip) {
    // 8462480737302404222943232 = 111 + 80 zeros. This should be stored as (0111 10100) where the
    // second value of 20 is the nibble shift of 4*20. The first value is 0111 because we store at
    // least 4 values. With skip this should be encoded as:
    // [(1111) (11111) (0111) (10100)] x3 [1000] = 83FEF4FFBD3FEF48
    uint128_t val = absl::MakeUint128(0x70000, 0x0);
    std::vector<boost::optional<uint128_t>> expectedInts;
    for (uint32_t i = 0; i < 3; i++) {
        expectedInts.push_back(val);
        expectedInts.push_back(boost::none);
    }
    std::vector<uint8_t> expectedBinary = {0x88, 0xF4, 0xFE, 0xD3, 0xFB, 0x4F, 0xEF, 0x3F};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Selector8LargeSkipEncoding) {
    // A perfect skip value is one that aligns perfectly with the boundary. 1111 with 124 zeros does
    // that.
    // This should be encoded as
    // [(001111 11111) x 5] [1001] [1000] = 1FF3FE7FCFF9FF98
    uint128_t val = absl::MakeUint128(0xF000000000000000, 0x0);
    std::vector<boost::optional<uint128_t>> expectedInts = {val, val, val, val, val};
    std::vector<uint8_t> expectedBinary = {0x98, 0xFF, 0xF9, 0xCF, 0x7F, 0xFE, 0xF3, 0x1F};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Selector8LargeNibbleShift) {
    // 170141183460469231731687303715884105728= 1 + 127 zeros. This is a value that should have 3
    // trailing zeros due to nibble. So we should encode as:
    // [(1000) (11111)] x6 [1000] [1000] = 23F1F8FC7E3F1F88
    uint128_t val = absl::MakeUint128(0x8000000000000000, 0x0);
    std::vector<boost::optional<uint128_t>> expectedInts = {val, val, val, val, val, val};
    std::vector<uint8_t> expectedBinary = {0x88, 0x1F, 0x3F, 0x7E, 0xFC, 0xF8, 0xF1, 0x23};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Test128WithSmallValue) {
    // This tests that if we use a small int128_t, we still correctly store.
    // 57344 = 1110000000000000 = 3 value bits and 13 zeros
    // This should be encoded as:
    // [(111) (1101)] x 8 [0010] [0111] = FBF7EFDFBF7EFD27
    uint128_t val = 57344;
    std::vector<boost::optional<uint128_t>> expectedInts = {val, val, val, val, val, val, val, val};
    std::vector<uint8_t> expectedBinary = {0x27, 0xFD, 0x7E, 0xBF, 0xDF, 0xEF, 0xF7, 0xFB};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, Selector7FullSelector1) {
    // Selector 7 value = 64 = 1 + 6 zeros.
    // We should encode this as:
    // [(01) (0110)] x 9 [0001] [0111] = 1659659659659617
    std::vector<boost::optional<uint64_t>> expectedInts(9, 64);

    // test that buffer was correct
    std::vector<uint8_t> expectedBinary = {0x17, 0x96, 0x65, 0x59, 0x96, 0x65, 0x59, 0x16};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, RleSevenSelector) {
    // Selector 7 value
    // 57344 = 3 value bits and 13 zeros
    // This should be encoded as
    // [(111) (1101)] x8 [0010] [0111] = FBF7EFDFBF7EFD27 + 0xF (rle) + repeat seven selector
    uint128_t val = 57344;
    std::vector<boost::optional<uint128_t>> expectedInts(136, val);
    std::vector<uint8_t> expectedBinary = {0x27, 0xFD, 0x7E, 0xBF, 0xDF, 0xEF, 0xF7, 0xFB,
                                           0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x27, 0xFD, 0x7E, 0xBF, 0xDF, 0xEF, 0xF7, 0xFB};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, RleEightSelectorSmall) {
    // Selector 8 value
    // 7340032 = 1110000000000000 = 3 value bits and 20 zeros
    // This should be encoded as
    // [(0111) (0101)] x7 [0001] [1000] = 7575757575757518 + 0xF (rle) + repeat eight selector
    uint128_t val = 7340032;
    std::vector<boost::optional<uint128_t>> expectedInts(134, val);
    std::vector<uint8_t> expectedBinary = {0x18, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75,
                                           0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x18, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, RleEightSelectorLarge) {
    // Selector 8 value
    // 8462480737302404222943232= 111 + 80 zeros
    // This should be encoded as
    // [(0111) (10000)] x6 [1000] [1000] = 1E8F47A3D1E8F488 + 0xF (rle) + repeat eight selector
    uint128_t val = absl::MakeUint128(0x70000, 0x0);
    std::vector<boost::optional<uint128_t>> expectedInts(132, val);
    std::vector<uint8_t> expectedBinary = {0x88, 0xF4, 0xE8, 0xD1, 0xA3, 0x47, 0x8F, 0x1E,
                                           0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x88, 0xF4, 0xE8, 0xD1, 0xA3, 0x47, 0x8F, 0x1E};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, RleFlushResetsRle) {
    BufBuilder buffer;
    Simple8bBuilder<uint64_t> builder([&buffer](uint64_t simple8bBlock) {
        buffer.appendNum(simple8bBlock);
        return true;
    });

    // Write a single 1 and flush. Then we add 120 more 1s and check that this does not start RLE.
    ASSERT_TRUE(builder.append(1));
    builder.flush();

    for (int i = 0; i < 120; ++i) {
        ASSERT_TRUE(builder.append(1));
    }
    builder.flush();

    auto size = buffer.len();
    auto sharedBuffer = buffer.release();

    std::vector<uint8_t> simple8bBlockOne1 = {0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    std::vector<uint8_t> simple8bBlockThirty1s = {0x52, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55};

    std::vector<uint8_t> expectedBinary;
    expectedBinary.insert(expectedBinary.end(), simple8bBlockOne1.begin(), simple8bBlockOne1.end());
    for (int i = 0; i < 4; ++i) {
        expectedBinary.insert(
            expectedBinary.end(), simple8bBlockThirty1s.begin(), simple8bBlockThirty1s.end());
    }

    ASSERT_EQ(size, expectedBinary.size());
    ASSERT_EQ(memcmp(sharedBuffer.get(), expectedBinary.data(), size), 0);

    Simple8b<uint64_t> s8b(sharedBuffer.get(), size);
    assertValuesEqual(s8b, std::vector<boost::optional<uint64_t>>(121, 1));
}

TEST(Simple8b, RleFlushResetsPossibleSelectors) {
    BufBuilder buffer;
    Simple8bBuilder<uint64_t> builder([&buffer](uint64_t simple8bBlock) {
        buffer.appendNum(simple8bBlock);
        return true;
    });

    // Write a large value with many trailing zeros that does not fit in the base selector, we then
    // flush and make sure that we can write a value that only fits in the base selector. We should
    // have reset possible selectors as part of the flush.
    std::vector<boost::optional<uint64_t>> expectedInts = {0x8000000000000000, 0x0FFFFFFFFFFFFFFE};

    ASSERT_TRUE(builder.append(*expectedInts[0]));
    builder.flush();
    ASSERT_TRUE(builder.append(*expectedInts[1]));
    builder.flush();

    auto size = buffer.len();
    auto sharedBuffer = buffer.release();

    Simple8b<uint64_t> s8b(sharedBuffer.get(), size);
    assertValuesEqual(s8b, expectedInts);
}

TEST(Simple8b, FlushResetsLastInPreviousWhenFlushingRle) {
    BufBuilder buffer;
    Simple8bBuilder<uint64_t> builder([&buffer](uint64_t simple8bBlock) {
        buffer.appendNum(simple8bBlock);
        return true;
    });

    // Write 150 1s and flush. This should result in a word with 30 1s followed by RLE. We make sure
    // that last value written is reset when RLE is the last thing we flush.
    for (int i = 0; i < 150; ++i) {
        ASSERT_TRUE(builder.append(1));
    }
    builder.flush();

    // Last value written is only used for RLE so append 120 values of the same value and make sure
    // this does _NOT_ start RLE as flush occured in between.
    for (int i = 0; i < 120; ++i) {
        ASSERT_TRUE(builder.append(1));
    }
    builder.flush();

    auto size = buffer.len();
    auto sharedBuffer = buffer.release();

    std::vector<uint8_t> simple8bBlockThirty1s = {0x52, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
    std::vector<uint8_t> simple8bBlockRLE = {0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    std::vector<uint8_t> expectedBinary = simple8bBlockThirty1s;
    expectedBinary.insert(expectedBinary.end(), simple8bBlockRLE.begin(), simple8bBlockRLE.end());
    for (int i = 0; i < 4; ++i) {
        expectedBinary.insert(
            expectedBinary.end(), simple8bBlockThirty1s.begin(), simple8bBlockThirty1s.end());
    }

    ASSERT_EQ(size, expectedBinary.size());
    ASSERT_EQ(memcmp(sharedBuffer.get(), expectedBinary.data(), size), 0);

    Simple8b<uint64_t> s8b(sharedBuffer.get(), size);
    assertValuesEqual(s8b, std::vector<boost::optional<uint64_t>>(270, 1));
}

TEST(Simple8b, FlushResetsLastInPreviousWhenFlushingRleZeroRleAfter) {
    BufBuilder buffer;
    Simple8bBuilder<uint64_t> builder([&buffer](uint64_t simple8bBlock) {
        buffer.appendNum(simple8bBlock);
        return true;
    });

    // Write 150 1s and flush. This should result in a word with 30 1s followed by RLE. We make sure
    // that last value written is reset when RLE is the last thing we flush.
    for (int i = 0; i < 150; ++i) {
        ASSERT_TRUE(builder.append(1));
    }
    builder.flush();
    auto sizeAfterFlush = buffer.len();

    // Write 120 0s. They should be encoded as a single RLE block.
    for (int i = 0; i < 120; ++i) {
        ASSERT_TRUE(builder.append(0));
    }
    builder.flush();

    auto size = buffer.len();
    auto sharedBuffer = buffer.release();

    std::vector<uint8_t> simple8bBlockThirty1s = {0x52, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
    std::vector<uint8_t> simple8bBlockRLE = {0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    std::vector<uint8_t> expectedBinary = simple8bBlockThirty1s;
    for (int i = 0; i < 2; ++i) {
        expectedBinary.insert(
            expectedBinary.end(), simple8bBlockRLE.begin(), simple8bBlockRLE.end());
    }

    ASSERT_EQ(size, expectedBinary.size());
    ASSERT_EQ(memcmp(sharedBuffer.get(), expectedBinary.data(), size), 0);

    {
        // Reading all values as one block would be interpreted as everything is 1s as we wrote a
        // RLE block immediately after a block containing 1 values.
        Simple8b<uint64_t> s8b(sharedBuffer.get(), size);
        assertValuesEqual(s8b, std::vector<boost::optional<uint64_t>>(270, 1));
    }

    // In practise the binary is split up in two parts where we can initialize the second part on
    // how the RLE should be interpreted.
    {
        Simple8b<uint64_t> s8b(sharedBuffer.get(), sizeAfterFlush);
        assertValuesEqual(s8b, std::vector<boost::optional<uint64_t>>(150, 1));
    }
    {
        Simple8b<uint64_t> s8b(
            sharedBuffer.get() + sizeAfterFlush, size - sizeAfterFlush, 0 /* previous */);
        assertValuesEqual(s8b, std::vector<boost::optional<uint64_t>>(120, 0));
    }
}

TEST(Simple8b, EightSelectorLargeMax) {
    // Selector 8 value
    // 1111 + 124 zeros
    // This should be encoded as
    // [001111] [11111]  x5 [1001] [1000] = 1FF3FE7FCFF9FF98
    uint128_t val = absl::MakeUint128(0xF000000000000000, 0x0);
    std::vector<boost::optional<uint128_t>> expectedInts(5, val);
    std::vector<uint8_t> expectedBinary = {0x98, 0xFF, 0xF9, 0xCF, 0x7F, 0xFE, 0xF3, 0x1f};
    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, RLELargeCount) {
    std::vector<boost::optional<uint64_t>> expectedInts(257 * 120, 0);

    std::vector<uint8_t> RLEblock16Count = {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    std::vector<uint8_t> RLEblock1Count = {0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    std::vector<uint8_t> expectedBinary;
    for (int i = 0; i < 16; ++i) {
        expectedBinary.insert(expectedBinary.end(), RLEblock16Count.begin(), RLEblock16Count.end());
    }
    expectedBinary.insert(expectedBinary.end(), RLEblock1Count.begin(), RLEblock1Count.end());

    testSimple8b(expectedInts, expectedBinary);
}

TEST(Simple8b, ValueTooLarge) {
    // This value needs 61 bits which it too large for Simple8b
    uint64_t value = 0x1FFFFFFFFFFFFFFF;
    Simple8bBuilder<uint64_t> builder([](uint64_t) {
        ASSERT(false);
        return true;
    });
    ASSERT_FALSE(builder.append(value));
}

TEST(Simple8b, ValueTooLargeMaxUInt64) {
    // Make sure we handle uint64_t max correctly.
    uint64_t value = std::numeric_limits<uint64_t>::max();

    Simple8bBuilder<uint64_t> builder([](uint64_t) {
        ASSERT(false);
        return true;
    });
    ASSERT_FALSE(builder.append(value));
}

TEST(Simple8b, ValueTooLargeMaxUInt128) {
    // Make sure we handle uint128_t max correctly.
    uint128_t value = std::numeric_limits<uint128_t>::max();

    Simple8bBuilder<uint128_t> builder([](uint64_t) {
        ASSERT(false);
        return true;
    });
    ASSERT_FALSE(builder.append(value));
}

TEST(Simple8b, ValueTooLargeMaxUInt64AsUInt128) {
    // Make sure we handle uint128_t max correctly.
    uint128_t value = std::numeric_limits<uint64_t>::max();

    Simple8bBuilder<uint128_t> builder([](uint64_t) {
        ASSERT(false);
        return true;
    });
    ASSERT_FALSE(builder.append(value));
}

TEST(Simple8b, ValueTooManyTrailingFor8SmallTooManyMeaningfulFor8Large) {
    // This value has 52 meaningful bits and 61 trailing zeros. This is too many trailing zeros for
    // Selector 8 Small and too many meaningful bits for Selector 8 Large.
    uint128_t value = absl::MakeUint128(0x1FFFFF0FFFFFF, 0xE000000000000000);
    Simple8bBuilder<uint128_t> builder([](uint64_t) {
        ASSERT(false);
        return true;
    });
    ASSERT_FALSE(builder.append(value));
}

TEST(Simple8b, ValueTooLargeMax8SmallAddForSkipPattern) {
    // This value has 52 meaningful bits and 60 trailing zeros. But one extra 0 needs to be added to
    // the meaningful bits to differentiate from the missing value pattern to be able to store in
    // Extended 8 Small which brings it to 53 bits which is too many. Extended 8 Large can't be used
    // either as it can only store 51 meaningful bits.
    uint128_t value = absl::MakeUint128(0xFFFFFFFFFFFF, 0xF000000000000000);
    Simple8bBuilder<uint128_t> builder([](uint64_t) {
        ASSERT(false);
        return true;
    });
    ASSERT_FALSE(builder.append(value));
}

TEST(Simple8b, ValueTooLargeTrailingZerosNotDivisibleBy4) {
    // This value has 52 meaningful bits and 59 trailing zeros. But 3 of the trailing bits need to
    // be stored in the data bits as it's not divisible by 4. This brings the data bits to 55 which
    // it too large.
    uint128_t value = absl::MakeUint128(0x7FFFFFFFFFFF, 0xF800000000000000);
    Simple8bBuilder<uint128_t> builder([](uint64_t) {
        ASSERT(false);
        return true;
    });
    ASSERT_FALSE(builder.append(value));
}

TEST(Simple8b, ValueTooLargeBitCountUsedForExtendedSelectors) {
    // This value has 63 meaningful bits and does not fit in Simple8b. When evaluating the extended
    // selectors it will almost fit as it can pack the 9 trailing zero in the count but the amount
    // of bits required will still be too large. Make sure append takes into the account the number
    // of bits used for the count when checking if the value can be stored.
    uint64_t value = 0x646075fffc000200;
    Simple8bBuilder<uint64_t> builder([](uint64_t) {
        ASSERT(false);
        return true;
    });
    ASSERT_FALSE(builder.append(value));
}
