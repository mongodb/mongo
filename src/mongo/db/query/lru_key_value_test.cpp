/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/query/lru_key_value.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

using namespace mongo;

/**
 * Tests LRU Key Value store with 2 budget estimators:
 * - trivial one gives a constant estimation for every entry;
 * - non-trivial one calculates an estimation basing on the entry's data.
 */
namespace {

//
// Convenience types and functions.
//

struct TrivialBudgetEstimator {
    size_t operator()(int) {
        return 1;
    }
};

using TestKeyValue = LRUKeyValue<int, int, TrivialBudgetEstimator>;

struct NonTrivialEntry {
    NonTrivialEntry(size_t key, size_t budgetSize) : key{key}, budgetSize{budgetSize} {}

    const size_t key;
    const size_t budgetSize;
};

struct NonTrivialBudgetEstimator {
    size_t operator()(const NonTrivialEntry& value) {
        return value.budgetSize;
    }
};

using NonTrivialTestKeyValue = LRUKeyValue<size_t, NonTrivialEntry, NonTrivialBudgetEstimator>;

template <typename Key, typename Value, typename Estimator>
void assertInKVStore(LRUKeyValue<Key, Value, Estimator>& cache, Key key, Value value) {
    ASSERT_TRUE(cache.hasKey(key));
    auto s = cache.get(key);
    ASSERT_OK(s);
    ASSERT_EQUALS(*s.getValue(), value);
}

template <typename Key, typename Value, typename Estimator>
void assertNotInKVStore(LRUKeyValue<Key, Value, Estimator>& cache, Key key) {
    ASSERT_FALSE(cache.hasKey(key));
    auto s = cache.get(key);
    ASSERT_NOT_OK(s);
}

/**
 * Test that we can add an entry and get it back out.
 */
TEST(LRUKeyValueTest, BasicAddGet) {
    TestKeyValue cache{100};
    cache.add(1, new int(2));
    assertInKVStore(cache, 1, 2);
}

/**
 * A kv-store with a max size of 0 isn't too useful, but test
 * that at the very least we don't blow up.
 */
TEST(LRUKeyValueTest, SizeZeroCache) {
    TestKeyValue cache{0};
    cache.add(1, new int(2));
    assertNotInKVStore(cache, 1);
}

/**
 * Make sure eviction and promotion work properly with a kv-store of size 1.
 */
TEST(LRUKeyValueTest, SizeOneCache) {
    TestKeyValue cache{1};
    cache.add(0, new int(0));
    assertInKVStore(cache, 0, 0);

    // Second entry should immediately evict the first.
    cache.add(1, new int(1));
    assertNotInKVStore(cache, 0);
    assertInKVStore(cache, 1, 1);
}

/**
 * Fill up a size 10 kv-store with 10 entries. Call get()
 * on every entry except for one. Then call add() and
 * make sure that the proper entry got evicted.
 */
TEST(LRUKeyValueTest, EvictionTest) {
    int maxSize = 10;
    TestKeyValue cache{static_cast<size_t>(maxSize)};
    for (int i = 0; i < maxSize; ++i) {
        auto nEvicted = cache.add(i, new int(i));
        ASSERT_EQ(0, nEvicted);
    }
    ASSERT_EQUALS(cache.size(), (size_t)maxSize);

    // Call get() on all but one key.
    int evictKey = 5;
    for (int i = 0; i < maxSize; ++i) {
        if (i == evictKey) {
            continue;
        }
        assertInKVStore(cache, i, i);
    }

    // Adding another entry causes an eviction.
    auto nEvicted = cache.add(maxSize + 1, new int(maxSize + 1));
    ASSERT_EQUALS(cache.size(), (size_t)maxSize);
    ASSERT_EQ(1ul, nEvicted);

    // Check that the least recently accessed has been evicted.
    for (int i = 0; i < maxSize; ++i) {
        if (i == evictKey) {
            assertNotInKVStore(cache, evictKey);
        } else {
            assertInKVStore(cache, i, i);
        }
    }
}

/**
 * Eviction test with non-trivial budget estimator.
 */
TEST(LRUKeyValueTest, EvictionTestWithNonTrivialEstimator) {
    constexpr size_t maxSize = 55;
    NonTrivialTestKeyValue cache{maxSize};
    size_t item = 0;
    // Adding entries {0, 1}, {1, 2} ... {9, 10} to the LRU store.
    for (; cache.size() + (item + 1) <= maxSize; ++item) {
        auto nEvicted = cache.add(item, new NonTrivialEntry{item, item + 1});
        ASSERT_EQ(0, nEvicted);
    }
    ASSERT_EQ(10u, item);
    ASSERT_EQ(maxSize, cache.size());

    size_t currentSize = maxSize;

    // The first 4 values should be evicted to cover the incloming entry with size = 7.
    constexpr size_t expectedToBeEvicted = 4;
    constexpr size_t sizeOfExpectedToBeEvictedItems = 1 + 2 + 3 + 4;

    constexpr size_t newItemKey = 17;
    constexpr size_t newItemSize = 7;

    currentSize += newItemSize - sizeOfExpectedToBeEvictedItems;

    auto nEvicted = cache.add(newItemKey, new NonTrivialEntry{newItemKey, newItemSize});

    ASSERT_EQ(expectedToBeEvicted, nEvicted);
    ASSERT_EQ(currentSize, cache.size());
}

TEST(LRUKeyValueTest, AddAnEntryWithBiggetThanBudgetSize) {
    constexpr size_t maxSize = 55;
    NonTrivialTestKeyValue cache{maxSize};
    size_t item = 0;
    for (; cache.size() + (item + 1) <= maxSize; ++item) {
        auto nEvicted = cache.add(item, new NonTrivialEntry{item, item + 1});
        ASSERT_EQ(0, nEvicted);
    }
    ASSERT_EQ(10u, item);
    ASSERT_EQ(maxSize, cache.size());

    auto nEvicted = cache.add(17, new NonTrivialEntry(15, 57));
    ASSERT_EQ(item + 1, nEvicted);  // all entries including the one just added must be evicted
    ASSERT_EQ(0ul, cache.size());   // the LRU store must be empty now
}

/**
 * Fill up a size 10 kv-store with 10 entries. Call get()
 * on a single entry to promote it to most recently
 * accessed. Then cause 9 evictions and make sure only
 * the entry on which we called get() remains.
 */
TEST(LRUKeyValueTest, PromotionTest) {
    int maxSize = 10;
    TestKeyValue cache{static_cast<size_t>(maxSize)};
    for (int i = 0; i < maxSize; ++i) {
        auto nEvicted = cache.add(i, new int(i));
        ASSERT_EQ(0, nEvicted);
    }
    ASSERT_EQUALS(cache.size(), (size_t)maxSize);

    // Call get() on a particular key.
    int promoteKey = 5;
    assertInKVStore(cache, promoteKey, promoteKey);

    // Evict all but one of the original entries.
    for (int i = maxSize; i < (maxSize + maxSize - 1); ++i) {
        auto nEvicted = cache.add(i, new int(i));
        ASSERT_GT(nEvicted, 0);
    }
    ASSERT_EQUALS(cache.size(), (size_t)maxSize);

    // Check that the promoteKey has not been evicted.
    for (int i = 0; i < maxSize; ++i) {
        if (i == promoteKey) {
            assertInKVStore(cache, promoteKey, promoteKey);
        } else {
            assertNotInKVStore(cache, i);
        }
    }
}

/**
 * Test that calling add() with a key that already exists
 * in the kv-store deletes the existing entry.
 */
TEST(LRUKeyValueTest, ReplaceKeyTest) {
    TestKeyValue cache{10};
    cache.add(4, new int(4));
    assertInKVStore(cache, 4, 4);
    cache.add(4, new int(5));
    assertInKVStore(cache, 4, 5);
}

/**
 * Test iteration over the kv-store.
 */
TEST(LRUKeyValueTest, IterationTest) {
    TestKeyValue cache{2};
    cache.add(1, new int(1));
    cache.add(2, new int(2));

    typedef std::list<std::pair<int, int*>>::const_iterator CacheIterator;
    CacheIterator i = cache.begin();
    ASSERT_EQUALS(i->first, 2);
    ASSERT_EQUALS(*i->second, 2);
    ++i;
    ASSERT_EQUALS(i->first, 1);
    ASSERT_EQUALS(*i->second, 1);
    ++i;
    ASSERT(i == cache.end());
}

}  // namespace
