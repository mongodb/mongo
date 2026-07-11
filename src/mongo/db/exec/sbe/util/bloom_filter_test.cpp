// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

