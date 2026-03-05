/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

/**
 * This file contains tests for sbe::BloomFilter.
 */

#include "mongo/db/exec/sbe/util/bloom_filter.h"

#include "mongo/unittest/framework.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

#include <absl/hash/hash.h>

namespace mongo::sbe {

class BloomFilterTestFixture : public unittest::Test {
protected:
    static constexpr size_t kSmallNumBytes = 1024;

    std::unique_ptr<SplitBlockBloomFilter> makeBloomFilter(size_t numBytes = kSmallNumBytes) {
        return std::make_unique<SplitBlockBloomFilter>(numBytes);
    }
};

TEST_F(BloomFilterTestFixture, InsertAndContains) {
    auto bf = makeBloomFilter();

    // Insert some keys
    bf->insert(absl::Hash<int>{}(10));
    bf->insert(absl::Hash<int>{}(20));
    bf->insert(absl::Hash<int>{}(30));

    // Check that inserted keys are found
    ASSERT_TRUE(bf->maybeContains(absl::Hash<int>{}(10)));
    ASSERT_TRUE(bf->maybeContains(absl::Hash<int>{}(20)));
    ASSERT_TRUE(bf->maybeContains(absl::Hash<int>{}(30)));
}

TEST_F(BloomFilterTestFixture, NonExistentKeyMayReturnFalse) {
    auto bf = makeBloomFilter();

    // Insert some keys
    for (int i = 0; i < 10; ++i) {
        bf->insert(absl::Hash<int>{}(i * 100));
    }

    // Check keys that were not inserted - at least some should return false
    // (Bloom filters can have false positives, but not false negatives)
    int falseCount = 0;
    for (int i = 0; i < 100; ++i) {
        if (!bf->maybeContains(absl::Hash<int>{}(i * 100 + 50))) {
            ++falseCount;
        }
    }

    // With a properly sized filter, we should see some definite negatives
    ASSERT_GT(falseCount, 0) << "Expected at least some definite negatives";
}

TEST_F(BloomFilterTestFixture, NoFalseNegatives) {
    auto bf = makeBloomFilter();

    // Insert many keys
    std::vector<int64_t> insertedKeys;
    for (int i = 0; i < 100; ++i) {
        insertedKeys.push_back(i);
        bf->insert(absl::Hash<int>{}(i));
    }

    // Verify no false negatives - all inserted keys must be found
    for (int64_t key : insertedKeys) {
        ASSERT_TRUE(bf->maybeContains(absl::Hash<int>{}(key))) << "False negative for key " << key;
    }
}

TEST_F(BloomFilterTestFixture, Clear) {
    auto bf = makeBloomFilter();

    // Insert some keys
    bf->insert(absl::Hash<int>{}(10));
    bf->insert(absl::Hash<int>{}(20));
    ASSERT_TRUE(bf->maybeContains(absl::Hash<int>{}(10)));
    ASSERT_TRUE(bf->maybeContains(absl::Hash<int>{}(20)));

    // clear
    bf->clear();

    // After clear, keys should not be found (no false negatives means
    // a clean filter should return false for anything)
    ASSERT_FALSE(bf->maybeContains(absl::Hash<int>{}(10)));
    ASSERT_FALSE(bf->maybeContains(absl::Hash<int>{}(20)));
}
}  // namespace mongo::sbe

