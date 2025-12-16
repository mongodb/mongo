/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/util/dynamic_bitset.h"

#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <numeric>
#include <vector>

#include <boost/dynamic_bitset.hpp>

namespace mongo {
namespace {
using Bitset = DynamicBitset<uint8_t, 1>;

TEST(DynamicBitsetTests, Constructors) {
    Bitset bitset1("0001010001");

    // Copy constructor
    Bitset bitset2(bitset1);
    ASSERT_EQ(bitset1, bitset2);

    // Move constructor
    Bitset bitset3(std::move(bitset2));
    ASSERT_EQ(bitset1, bitset3);

    // Copy assignment
    Bitset bitset4("0000000010");
    bitset4 = bitset1;
    ASSERT_EQ(bitset1, bitset4);

    // Move assignment
    Bitset bitset5("0000000010");
    bitset5 = std::move(bitset4);
    ASSERT_EQ(bitset1, bitset5);
}

TEST(DynamicBitsetTests, And) {
    ASSERT_EQ(Bitset("00000000"), Bitset("00000000") & Bitset("11111111"));
    ASSERT_EQ(Bitset("00010000"), Bitset("00010000") & Bitset("11111111"));
    ASSERT_EQ(Bitset("000101000"), Bitset("100101001") & Bitset("000111000"));
}

TEST(DynamicBitsetTests, Or) {
    ASSERT_EQ(Bitset("11111111"), Bitset("0000000") | Bitset("11111111"));
    ASSERT_EQ(Bitset("11111111"), Bitset("0001000") | Bitset("11111111"));
    ASSERT_EQ(Bitset("100111001"), Bitset("100101001") | Bitset("000111000"));
}

TEST(DynamicBitsetTests, Xor) {
    ASSERT_EQ(Bitset("11111111"), Bitset("00000000") ^ Bitset("11111111"));
    ASSERT_EQ(Bitset("11101111"), Bitset("00010000") ^ Bitset("11111111"));
    ASSERT_EQ(Bitset("100010001"), Bitset("100101001") ^ Bitset("000111000"));
}

TEST(DynamicBitsetTests, SetDifference) {
    ASSERT_EQ(Bitset("11111111"), Bitset("11111111") - Bitset("00000000"));
    ASSERT_EQ(Bitset("00000000"), Bitset("00010000") - Bitset("11111111"));
    ASSERT_EQ(Bitset("100000001"), Bitset("100101001") - Bitset("000111000"));
}

TEST(DynamicBitsetTests, Not) {
    ASSERT_EQ(Bitset("00000000"), ~Bitset("11111111"));
    ASSERT_EQ(Bitset("00010000"), ~Bitset("11101111"));
    ASSERT_EQ(Bitset("1110001110000000"), ~Bitset("0001110001111111"));
}

TEST(DynamicBitsetTests, GetUsingTest) {
    Bitset bitset("100001010111001");
    ASSERT_TRUE(bitset.test(0));
    ASSERT_FALSE(bitset.test(1));
    ASSERT_TRUE(bitset.test(4));
    ASSERT_TRUE(bitset.test(5));
    ASSERT_FALSE(bitset.test(6));
    ASSERT_TRUE(bitset.test(9));
    ASSERT_FALSE(bitset.test(10));
    ASSERT_TRUE(bitset.test(14));
}

TEST(DynamicBitsetTests, SetAndGetSmall) {
    Bitset bitset(8);

    bitset.set(0, true);
    ASSERT_EQ(Bitset("00000001"), bitset);
    ASSERT_TRUE(bitset[0]);

    bitset.set(0, false);
    ASSERT_EQ(Bitset("00000000"), bitset);
    ASSERT_FALSE(bitset[0]);

    bitset[1] = true;
    ASSERT_TRUE(bitset[1]);
    bitset[2] = false;
    ASSERT_FALSE(bitset[2]);
    bitset[3] = 1;
    ASSERT_TRUE(bitset[3]);
    bitset[4] = 0;
    ASSERT_FALSE(bitset[4]);
    bitset[4].flip();
    ASSERT_TRUE(bitset[4]);
    ASSERT_FALSE(~bitset[4]);

    ASSERT_EQ(Bitset("00011010"), bitset);
}

TEST(DynamicBitsetTests, SetAndGetLargeBitset) {
    Bitset largeBitset(32);
    largeBitset.set(0, true);
    largeBitset.set(31, true);
    ASSERT_TRUE(largeBitset[0]);
    ASSERT_TRUE(largeBitset[31]);
    ASSERT_EQ(Bitset("10000000000000000000000000000001"), largeBitset);

    largeBitset[1] = true;
    largeBitset[30] = true;
    ASSERT_EQ(Bitset("11000000000000000000000000000011"), largeBitset);

    largeBitset.set(0, false);
    largeBitset.set(31, false);
    largeBitset.set(2, true);
    ASSERT_EQ(Bitset("01000000000000000000000000000110"), largeBitset);
}

TEST(DynamicBitsetTests, Clear) {
    {
        Bitset bitset("11111111111111111111111111111111");
        bitset.clear();
        ASSERT_EQ(bitset, Bitset(32));
    }
    {
        Bitset bitset("10000001000000100001000000000001");
        bitset.clear();
        ASSERT_EQ(bitset, Bitset(32));
    }
}

TEST(DynamicBitsetTests, SetAll) {
    {
        Bitset bitset(32);
        bitset.set();
        ASSERT_EQ(~Bitset(32), bitset);
    }
    {
        Bitset bitset("10000001000000100001000000000001");
        bitset.set();
        ASSERT_EQ(~Bitset(32), bitset);
    }
}

TEST(DynamicBitsetTests, Count) {
    ASSERT_EQ(8, Bitset("11111111").count());
    ASSERT_EQ(0, Bitset("00000000").count());
    ASSERT_EQ(7, Bitset("11101111").count());
    ASSERT_EQ(1, Bitset("00010000").count());
    ASSERT_EQ(10, Bitset("1111111111").count());
    ASSERT_EQ(3, Bitset("100010001").count());
    ASSERT_EQ(4, Bitset("100101001").count());
    ASSERT_EQ(3, Bitset("000111000").count());
}

TEST(DynamicBitsetTests, Size) {
    ASSERT_EQ(8, Bitset("11111111").size());
    ASSERT_EQ(8, Bitset("0").size());
    ASSERT_EQ(8, Bitset("11101111").size());
    ASSERT_EQ(16, Bitset("1111111111").size());
    ASSERT_EQ(32, (DynamicBitset<uint32_t, 2>(1).size()));
    ASSERT_EQ(64, (DynamicBitset<uint64_t, 2>(1).size()));
}

TEST(DynamicBitsetTests, IsSubsetOf) {
    ASSERT_TRUE(Bitset("0000000000").isSubsetOf(Bitset("0000000000")));
    ASSERT_TRUE(Bitset("0000000001").isSubsetOf(Bitset("0000000001")));
    ASSERT_TRUE(Bitset("0000000000").isSubsetOf(Bitset("1111111111")));
    ASSERT_TRUE(Bitset("0000000001").isSubsetOf(Bitset("1111111111")));
    ASSERT_TRUE(Bitset("1000000000").isSubsetOf(Bitset("1111111111")));
    ASSERT_TRUE(Bitset("0000010000").isSubsetOf(Bitset("1111111111")));
    ASSERT_TRUE(Bitset("0000010000").isSubsetOf(Bitset("1111111111")));
    ASSERT_TRUE(Bitset("0010100000").isSubsetOf(Bitset("1010101010")));
    ASSERT_FALSE(Bitset("1000000000").isSubsetOf(Bitset("0111111111")));
    ASSERT_FALSE(Bitset("0000000001").isSubsetOf(Bitset("1111111110")));
    ASSERT_FALSE(Bitset("0011100000").isSubsetOf(Bitset("1010101010")));
}

TEST(DynamicBitsetTests, Intersects) {
    ASSERT_FALSE(Bitset("0000000000").intersects(Bitset("0000000000")));
    ASSERT_TRUE(Bitset("0000000001").intersects(Bitset("0000000001")));
    ASSERT_FALSE(Bitset("0000000000").intersects(Bitset("1111111111")));
    ASSERT_TRUE(Bitset("0000000001").intersects(Bitset("1111111111")));
    ASSERT_TRUE(Bitset("1000000000").intersects(Bitset("1111111111")));
    ASSERT_FALSE(Bitset("1000000000").intersects(Bitset("0111111111")));
}

TEST(DynamicBitsetTests, isEqualToMasked) {
    ASSERT_TRUE(Bitset("0000000000").isEqualToMasked(Bitset("0011100000"), Bitset("0000000000")));
    ASSERT_TRUE(Bitset("0010000000").isEqualToMasked(Bitset("0011100000"), Bitset("0010000000")));
    ASSERT_TRUE(Bitset("0011100000").isEqualToMasked(Bitset("0011100000"), Bitset("1111111111")));
}

TEST(DynamicBitsetTests, FindFirst) {
    ASSERT_EQ(Bitset::npos, Bitset("0000000000").findFirst());
    ASSERT_EQ(0, Bitset("0000000001").findFirst());
    ASSERT_EQ(9, Bitset("1000000000").findFirst());
    ASSERT_EQ(4, Bitset("1000010000").findFirst());
}

TEST(DynamicBitsetTests, FindNext) {
    ASSERT_EQ(5, Bitset("0000100000").findNext(0));
    ASSERT_EQ(6, Bitset("0001100000").findNext(5));
    ASSERT_EQ(8, Bitset("0100100000").findNext(5));
    ASSERT_EQ(2, Bitset("10001010010010101").findNext(0));
    ASSERT_EQ(4, Bitset("10001010010010101").findNext(2));
    ASSERT_EQ(7, Bitset("10001010010010101").findNext(4));
    ASSERT_EQ(10, Bitset("10001010010010101").findNext(7));
    ASSERT_EQ(12, Bitset("10001010010010101").findNext(10));
    ASSERT_EQ(16, Bitset("10001010010010101").findNext(12));
    ASSERT_EQ(Bitset::npos, Bitset("10001010010010101").findNext(16));
}

TEST(DynamicBitsetTests, FindAllSetBits) {
    constexpr size_t size = 128;
    Bitset bitset(size);
    bitset.flip();
    size_t expected = 0;
    for (size_t i = bitset.findFirst(); i < bitset.size(); i = bitset.findNext(i)) {
        ASSERT_EQ(expected++, i);
    }
    ASSERT_EQ(size, expected);
}

TEST(DynamicBitsetTests, Resize) {
    // Inlined
    {
        Bitset bitset("10000001");
        Bitset expected("00000001");

        ASSERT_EQ(8, bitset.size());

        bitset.resize(1);  // will be resized to 8
        ASSERT_EQ(8, bitset.size());

        ASSERT_EQ(expected, bitset);
    }

    // Inlined -> on heap.
    {
        Bitset bitset("10000001");
        Bitset expected("0000000010000001");

        ASSERT_EQ(8, bitset.size());

        bitset.resize(10);  // will be resized to 16
        ASSERT_EQ(16, bitset.size());

        ASSERT_EQ(expected, bitset);
    }

    // On heap -> inlined.
    {
        Bitset bitset("0000000011111011");
        Bitset expected("00001011");

        ASSERT_EQ(16, bitset.size());

        bitset.resize(4);
        ASSERT_EQ(8, bitset.size());


        ASSERT_EQ(expected, bitset);
    }

    // Shrink, on heap -> on heap.
    {
        Bitset bitset("111111110000100011110111");
        Bitset expected("00100011110111");

        ASSERT_EQ(24, bitset.size());

        bitset.resize(12);
        ASSERT_EQ(16, bitset.size());

        ASSERT_EQ(expected, bitset);
    }
}

TEST(DynamicBitsetTests, Any) {
    ASSERT_TRUE(Bitset("11111111").any());
    ASSERT_TRUE((~Bitset(16)).any());
    ASSERT_TRUE((~Bitset(32)).any());
    ASSERT_TRUE(Bitset("11110111").any());
    ASSERT_FALSE(Bitset(16).any());
}

TEST(DynamicBitsetTests, None) {
    ASSERT_TRUE(Bitset(16).none());
    ASSERT_TRUE(Bitset("00000000").none());
    ASSERT_FALSE((~Bitset(32)).none());
    ASSERT_FALSE(Bitset("11110111").none());
}

TEST(DynamicBitsetTests, All) {
    ASSERT_FALSE(Bitset("1").all());
    ASSERT_TRUE(Bitset("11111111").all());
    ASSERT_FALSE(Bitset("111111111111").all());
    ASSERT_TRUE(Bitset("1111111111111111").all());
}

template <typename BlockType, size_t nBlocks>
DynamicBitset<BlockType, nBlocks> makeBitset(size_t n) {
    DynamicBitset<BlockType, nBlocks> bitset(sizeof(size_t) * CHAR_BIT);
    // Iterates through each set bit in the number 'n': gets the index of the next set bit  and
    // clears the bit until 'n' becomes 0.
    for (; n != 0; n &= n - 1)
        bitset.set(std::countr_zero(n));
    return bitset;
}

void testAllInPrefix(size_t n, size_t maxBits) {
    auto mongoBitset = makeBitset<uint8_t, 8>(n);
    boost::dynamic_bitset<size_t> boostBitset(maxBits, n);
    for (; !boostBitset.empty(); boostBitset.pop_back())
        ASSERT_EQ(mongoBitset.allInPrefix(boostBitset.size()), boostBitset.all());
    ASSERT_TRUE(mongoBitset.allInPrefix(0));
}

TEST(DynamicBitsetTests, allInPrefix) {
    static constexpr size_t maxBits = 20;
    static constexpr size_t maxValue = 1ull << maxBits;

    for (size_t n = 0; n != maxValue; ++n)
        testAllInPrefix(n, maxBits);
}

TEST(DynamicBitsetTests, Less) {
    ASSERT_LT(Bitset("0000"), Bitset("0001"));
    ASSERT_LT(Bitset("100000000"), Bitset("010000000"));
    ASSERT_LT(Bitset("0000"), Bitset("000000000"));
    ASSERT_LT(Bitset("100000000"), Bitset("1010000000"));
    ASSERT_FALSE(Bitset("0001") < Bitset("0001"));
}

template <typename T, size_t nBlocks>
void testDynamicBitsetPopulationView(std::vector<size_t> expectedBits) {
    auto maxIndexPos = std::max_element(expectedBits.begin(), expectedBits.end());
    const size_t size = maxIndexPos == expectedBits.end() ? 0 : *maxIndexPos + 1;
    DynamicBitset<T, nBlocks> bitset(size);
    for (size_t bitIndex : expectedBits)
        bitset.set(bitIndex);
    auto view = makePopulationView(bitset);
    ASSERT_EQ(std::vector<size_t>(view.begin(), view.end()), expectedBits);
}

template <typename T, size_t nBlocks>
void testDynamicBitsetPopulationView() {
    testDynamicBitsetPopulationView<T, nBlocks>({});
    testDynamicBitsetPopulationView<T, nBlocks>({0});
    testDynamicBitsetPopulationView<T, nBlocks>({1});
    testDynamicBitsetPopulationView<T, nBlocks>({0, 1});
    testDynamicBitsetPopulationView<T, nBlocks>({0, 74, 90});
    testDynamicBitsetPopulationView<T, nBlocks>({2, 4, 37, 50, 70});

    std::vector<size_t> allBitsSet(100);
    std::iota(allBitsSet.begin(), allBitsSet.end(), 0);
    testDynamicBitsetPopulationView<T, nBlocks>(std::move(allBitsSet));
}

void testPartiallyPopulatedDynamicBitsetPopulationView(size_t numPopulatedBlocks,
                                                       size_t firstBlockToPopulate,
                                                       bool addZeroBitsAfterPopulatedBlocks) {
    using BlockType = size_t;
    constexpr size_t kBlockSize = sizeof(BlockType) * CHAR_BIT;

    std::vector<size_t> setbits{kBlockSize * numPopulatedBlocks};
    std::iota(setbits.begin(), setbits.end(), kBlockSize * firstBlockToPopulate);
    if (addZeroBitsAfterPopulatedBlocks)
        setbits.push_back(setbits.back() + 10);
    testDynamicBitsetPopulationView<BlockType, 1>(std::move(setbits));
}

TEST(DynamicBitsetPopulationViewTests, Iterator) {
    testDynamicBitsetPopulationView<uint8_t, 1>();
    testDynamicBitsetPopulationView<uint8_t, 2>();
    testDynamicBitsetPopulationView<uint8_t, 8>();
    testDynamicBitsetPopulationView<uint64_t, 1>();
    testDynamicBitsetPopulationView<uint64_t, 2>();
    testDynamicBitsetPopulationView<uint64_t, 8>();
}

/**
 * Only one block is fully filled and it's the starting block.
 */
TEST(DynamicBitsetPopulationViewTests, IteratorFullStartingBlock) {
    testPartiallyPopulatedDynamicBitsetPopulationView(
        /*numPopulatedBlocks*/ 1,
        /*firstBlockToPopulate*/ 0,
        /*addZeroBitsAfterPopulatedBlocks*/ true);
}

/**
 * Some block in the middle is fully filled.
 */
TEST(DynamicBitsetPopulationViewTests, IteratorFullMiddleBlock) {
    testPartiallyPopulatedDynamicBitsetPopulationView(
        /*numPopulatedBlocks*/ 1,
        /*firstBlockToPopulate*/ 1,
        /*addZeroBitsAfterPopulatedBlocks*/ true);
}

/**
 * Final block is fully filled.
 */
TEST(DynamicBitsetPopulationViewTests, IteratorFullFinalBlock) {
    testPartiallyPopulatedDynamicBitsetPopulationView(
        /*numPopulatedBlocks*/ 1,
        /*firstBlockToPopulate*/ 2,
        /*addZeroBitsAfterPopulatedBlocks*/ false);
}

/**
 * Two consecutive blocks are filled.
 */
TEST(DynamicBitsetPopulationViewTests, IteratorFullTwoBlocks) {
    testPartiallyPopulatedDynamicBitsetPopulationView(
        /*numPopulatedBlocks*/ 2,
        /*firstBlockToPopulate*/ 1,
        /*addZeroBitsAfterPopulatedBlocks*/ true);
}
}  // namespace
}  // namespace mongo
