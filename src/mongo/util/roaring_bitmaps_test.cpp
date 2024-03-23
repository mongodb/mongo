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

#include "mongo/unittest/unittest.h"
#include "mongo/util/roaring_bitmaps.h"

#include <absl/container/flat_hash_set.h>

namespace mongo {
TEST(RoaringBitmapTest, Roaring64BTreeAddChecked) {
    Roaring64BTree roaring64;

    // This set doubles the tested Roaring64BTree to make sure that roaring64 behaves correctly.
    absl::flat_hash_set<uint64_t> doubleSet;

    uint64_t number = 1;

    // First loop: add 2^x numbers.
    for (uint64_t i = 1; i < 64; ++i) {
        bool isNew = doubleSet.insert(number).second;
        ASSERT_EQ(isNew, roaring64.addChecked(number));
        number <<= 1;
    }

    // Second loop: add divisors of 4.
    for (uint64_t i = 0; i < 4000; i += 4) {
        bool isNew = doubleSet.insert(i).second;
        ASSERT_EQ(isNew, roaring64.addChecked(i));
    }

    // Third loop: Add numbers from 100 to 700.
    for (uint64_t i = 100; i < 700; ++i) {
        bool isNew = doubleSet.insert(i).second;
        ASSERT_EQ(isNew, roaring64.addChecked(i));
    }
}

TEST(RoaringBitmapTest, Roaring64BTreeAdd) {
    Roaring64BTree roaring64;

    uint64_t number = 1;

    // First loop: add 2^x numbers.
    for (uint64_t i = 1; i < 64; ++i) {
        roaring64.add(number);
        number <<= 1;
    }

    // Second loop: add divisors of 4.
    for (uint64_t i = 0; i < 4000; i += 4) {
        roaring64.add(i);
    }

    // Third loop: Add numbers from 100 to 700.
    for (uint64_t i = 100; i < 700; ++i) {
        roaring64.add(i);
    }

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
}  // namespace mongo
