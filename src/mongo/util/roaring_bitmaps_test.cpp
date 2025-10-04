/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/util/roaring_bitmaps.h"

#include "mongo/unittest/unittest.h"

#include <absl/container/flat_hash_set.h>

namespace mongo {
TEST(RoaringBitmapTest, Roaring64BTreeAddChecked) {
    Roaring64BTree roaring64;

    // Stores the key that roaring uses to map to containers.
    absl::flat_hash_set<uint16_t> roaringKeys;
    // Stores the keys that Roaring64BTree uses internally to map roarings.
    absl::flat_hash_set<uint32_t> mapKeys;
    // Stores the seen values.
    absl::flat_hash_set<uint64_t> keys;
    uint64_t expectedSize{0};

    auto updateSize = [&](uint64_t value) {
        uint32_t mapKey = static_cast<uint32_t>(value >> 32);
        uint32_t roaringKey = value & 0xFFFFFFFFFFFF0000ULL;
        // Account for the key in _roarings.
        expectedSize += sizeof(uint32_t) * (mapKeys.insert(mapKey).second ? 1 : 0);
        // Account for the key in _existingContainers + 2 bytes for the key for the container in
        // roaring.
        expectedSize += (2 + sizeof(uint64_t)) * (roaringKeys.insert(roaringKey).second ? 1 : 0);
        // Account for the value in roaring.
        expectedSize += sizeof(uint16_t) * (keys.insert(value).second ? 1 : 0);
    };

    auto assertSize = [&]() {
        auto approximateSize = roaring64.getApproximateSize();
        auto computedSize = roaring64.computeMemorySize();
        ASSERT_EQ(approximateSize, expectedSize);
        ASSERT_GTE(approximateSize, 0.5 * computedSize);
        ASSERT_LTE(approximateSize, 1.5 * computedSize);
    };

    // This set doubles the tested Roaring64BTree to make sure that roaring64 behaves correctly.
    absl::flat_hash_set<uint64_t> doubleSet;

    uint64_t number = 1;

    // First loop: add 2^x numbers.
    for (uint64_t i = 1; i < 64; ++i) {
        bool isNew = doubleSet.insert(number).second;
        ASSERT_EQ(isNew, roaring64.addChecked(number));
        updateSize(number);
        number <<= 1;
    }
    assertSize();

    // Second loop: add divisors of 4.
    for (uint64_t i = 0; i < 4000; i += 4) {
        bool isNew = doubleSet.insert(i).second;
        ASSERT_EQ(isNew, roaring64.addChecked(i));
        updateSize(i);
    }
    assertSize();

    // Third loop: Add numbers from 100 to 700.
    for (uint64_t i = 100; i < 700; ++i) {
        bool isNew = doubleSet.insert(i).second;
        ASSERT_EQ(isNew, roaring64.addChecked(i));
    }
}

TEST(RoaringBitmapTest, Roaring64BTreeAdd) {
    Roaring64BTree roaring64;

    // Stores the key that roaring uses to map to containers.
    absl::flat_hash_set<uint16_t> roaringKeys;
    // Stores the keys that Roaring64BTree uses internally to map roarings.
    absl::flat_hash_set<uint32_t> mapKeys;
    // Stores the seen values.
    absl::flat_hash_set<uint64_t> keys;
    uint64_t expectedSize{0};

    auto updateSize = [&](uint64_t value) {
        uint32_t mapKey = static_cast<uint32_t>(value >> 32);
        uint32_t roaringKey = value & 0xFFFFFFFFFFFF0000ULL;
        // Account for the key in _roarings.
        expectedSize += sizeof(uint32_t) * (mapKeys.insert(mapKey).second ? 1 : 0);
        // Account for the key in _existingContainers + 2 bytes for the key for the container in
        // roaring.
        expectedSize += (2 + sizeof(uint64_t)) * (roaringKeys.insert(roaringKey).second ? 1 : 0);
        // Account for the value in roaring.
        expectedSize += sizeof(uint16_t) * (keys.insert(value).second ? 1 : 0);
    };

    auto assertSize = [&]() {
        auto approximateSize = roaring64.getApproximateSize();
        auto computedSize = roaring64.computeMemorySize();
        ASSERT_EQ(approximateSize, expectedSize);
        ASSERT_GTE(approximateSize, 0.5 * computedSize);
        ASSERT_LTE(approximateSize, 1.5 * computedSize);
    };

    uint64_t number = 1;

    // First loop: add 2^x numbers.
    for (uint64_t i = 1; i < 64; ++i) {
        roaring64.add(number);
        updateSize(number);
        number <<= 1;
    }
    assertSize();

    // Second loop: add divisors of 4.
    for (uint64_t i = 0; i < 4000; i += 4) {
        roaring64.add(i);
        updateSize(i);
    }
    assertSize();

    // Third loop: Add numbers from 100 to 700.
    for (uint64_t i = 100; i < 700; ++i) {
        roaring64.add(i);
        updateSize(i);
    }
    assertSize();

    // Now verify that all the number above have been added.

    number = 1;
    // First loop: check 2^x numbers.
    for (uint64_t i = 1; i < 64; ++i) {
        ASSERT_TRUE(roaring64.contains(number));
        number <<= 1;
    }

    // Second loop: check divisors of 4.
    for (uint64_t i = 0; i < 4000; i += 4) {
        ASSERT_TRUE(roaring64.contains(i));
    }

    // Third loop: check numbers from 100 to 700.
    for (uint64_t i = 100; i < 700; ++i) {
        ASSERT_TRUE(roaring64.contains(i));
    }
}

TEST(RoaringBitmapTest, Roaring64BTreeIterator) {
    Roaring64BTree roaring64;

    // Generate the numbers to add to the set
    mongo::stdx::unordered_set<uint64_t> addedNums;
    uint64_t num = 1;
    for (uint64_t i = 0; i < 64; ++i) {
        num <<= 1;
        for (uint64_t j = 0; j < 10; ++j) {
            uint64_t number = num + j * 8;
            addedNums.insert(number);
        }
    }

    // Add the numbers to the set
    for (const auto& number : addedNums) {
        roaring64.add(number);
    }


    // Check the numbers retrieved are the same that were inserted and they are retrieved in a
    // sorted order.
    uint64_t foundNum = 0;
    uint64_t previousNumber = 0;
    for (auto it = roaring64.begin(); it != roaring64.end(); ++it) {
        uint64_t number = *it;
        ASSERT(addedNums.contains(number));
        if (foundNum) {
            ASSERT_GT(number, previousNumber);
        }
        previousNumber = number;
        ++foundNum;
    }

    // Check that number of values retrieved are the same as the number of values inserted.
    ASSERT_EQ(foundNum, addedNums.size());

    // Check again using for-each loop to make sure that it works as well.
    foundNum = 0;
    previousNumber = 0;
    for (const auto& number : roaring64) {
        ASSERT(addedNums.contains(number));
        if (foundNum) {
            ASSERT_GT(number, previousNumber);
        }
        previousNumber = number;
        ++foundNum;
    }

    // Check that number of values retrieved are the same as the number of values inserted.
    ASSERT_EQ(foundNum, addedNums.size());
}
}  // namespace mongo
