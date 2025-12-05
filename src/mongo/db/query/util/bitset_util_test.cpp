/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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


#include "mongo/db/query/util/bitset_util.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
/**
 * Sets the bits specified by 'expectedBits' and asserts that the 'iterable' function correctly
 * returns all of their indices. The 'size' parameter is passed to the iterable function to test its
 * optimization behavior.
 */
template <size_t N>
void assertBitsetIterator(std::vector<size_t> expectedBits, size_t size = N) {
    std::bitset<N> bitset;
    for (size_t bitIndex : expectedBits) {
        bitset.set(bitIndex);
    }

    for (auto bitIndex : iterable(bitset, size)) {
        ASSERT_TRUE(bitset[bitIndex]);
    }

    std::sort(begin(expectedBits), end(expectedBits));
    std::vector<size_t> actualBits(begin(bitset), end(bitset));

    ASSERT_EQ(actualBits, expectedBits);
}

TEST(BitsetIteratorTest, Bitset64) {
    assertBitsetIterator<64>({});                // empty array
    assertBitsetIterator<64>({0});               // first bit set
    assertBitsetIterator<64>({1});               // first bit set
    assertBitsetIterator<64>({0, 1});            // first and second bits set
    assertBitsetIterator<64>({0, 63});           // first and last bits set
    assertBitsetIterator<64>({10, 17, 20, 63});  // multiple bits

    std::vector<size_t> allBits(64);
    ASSERT_EQ(allBits.size(), 64);
    iota(begin(allBits), end(allBits), 0);
    ASSERT_EQ(allBits[63], 63);
    assertBitsetIterator<64>(allBits);  // all bits set
}

TEST(BitsetIteratorTest, Bitset64Size8) {
    assertBitsetIterator<64>({}, 8);         // empty array
    assertBitsetIterator<64>({0}, 8);        // first bit set
    assertBitsetIterator<64>({1}, 8);        // first bit set
    assertBitsetIterator<64>({0, 1}, 8);     // first and second bits set
    assertBitsetIterator<64>({0, 7}, 8);     // first and last bits set
    assertBitsetIterator<64>({2, 4, 5}, 8);  // multiple bits

    std::vector<size_t> allBits(8);
    ASSERT_EQ(allBits.size(), 8);
    iota(begin(allBits), end(allBits), 0);
    ASSERT_EQ(allBits[7], 7);
    assertBitsetIterator<64>(allBits, 8);  // all bits set
}

TEST(BitsetIteratorTest, Bitset200Size75) {
    assertBitsetIterator<200>({}, 75);                  // empty array
    assertBitsetIterator<200>({0}, 75);                 // first bit set
    assertBitsetIterator<200>({1}, 75);                 // first bit set
    assertBitsetIterator<200>({0, 1}, 75);              // first and second bits set
    assertBitsetIterator<200>({0, 74}, 75);             // first and last bits set
    assertBitsetIterator<200>({2, 4, 50, 37, 70}, 75);  // multiple bits

    std::vector<size_t> allBits(75);
    ASSERT_EQ(allBits.size(), 75);
    iota(begin(allBits), end(allBits), 0);
    ASSERT_EQ(allBits[74], 74);
    assertBitsetIterator<200>(allBits, 75);  // all bits set
}

template <size_t N>
void assertLess(std::string_view lhs, std::string_view rhs) {
    std::bitset<N> lhb{lhs.data(), lhs.size()};
    std::bitset<N> lhb2{lhs.data(), lhs.size()};
    std::bitset<N> rhb{rhs.data(), rhs.size()};
    ASSERT_TRUE(bitsetLess(lhb, rhb));
    ASSERT_FALSE(bitsetLess(rhb, lhb));
    ASSERT_FALSE(bitsetLess(lhb, lhb));
    ASSERT_FALSE(bitsetLess(lhb, lhb2));
}

TEST(BitsetCompareTest, Less) {
    assertLess<8>("0", "1");
    assertLess<32>("0", "1");
    assertLess<64>("0", "1");
    assertLess<100>("0", "1");


    assertLess<8>("10", "11");
    assertLess<32>("10", "11");
    assertLess<64>("10", "11");
    assertLess<100>("10", "11");

    assertLess<32>("1000000000010", "1000000000011");
    assertLess<64>("1000000000010", "1000000000011");
    assertLess<100>("1000000000010", "1000000000011");
}
}  // namespace
}  // namespace mongo
