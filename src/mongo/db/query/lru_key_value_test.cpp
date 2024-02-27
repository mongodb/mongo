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

#include <memory>
#include <ostream>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>

#include "mongo/base/string_data.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

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

struct ValueType {
    bool operator==(const ValueType& other) const {
        return val == other.val;
    }

    bool operator!=(const ValueType& other) const {
        return !(*this == other);
    }

    friend std::ostream& operator<<(std::ostream& stream, const ValueType& val) {
        return stream << val.val;
    }

    int val;
};

struct TrivialBudgetEstimator {
    static constexpr size_t kSize = 1;

    size_t operator()(const int&, const ValueType&) {
        return kSize;
    }

    size_t operator()(const int&, const std::unique_ptr<int>&) {
        return kSize;
    }

    size_t operator()(const int&, const std::shared_ptr<int>) {
        return kSize;
    }
};

using TestSharedPtrValue = LRUKeyValue<int, std::shared_ptr<int>, TrivialBudgetEstimator>;

struct NonTrivialEntry {
    NonTrivialEntry(size_t key, size_t budgetSize) : key{key}, budgetSize{budgetSize} {}

    const size_t key;
    const size_t budgetSize;
};

struct NonTrivialBudgetEstimator {
    size_t operator()(const int& key, const std::shared_ptr<NonTrivialEntry> value) {
        return value->budgetSize;
    }
};

using NonTrivialTestSharedPtrValue =
    LRUKeyValue<size_t, std::shared_ptr<NonTrivialEntry>, NonTrivialBudgetEstimator>;

class NonTrivialInsertionEvictionListener {
public:
    NonTrivialInsertionEvictionListener() {
        keyTotal = 0;
        valueTotal = 0;
        budgetTotal = 0;
    }

    void onInsert(const int& k, const ValueType& v, size_t budget) {
        keyTotal += k;
        valueTotal += v.val;
        budgetTotal += budget;
    }

    void onEvict(const int& k, const ValueType& v, size_t budget) {
        keyTotal -= k;
        valueTotal -= v.val;
        budgetTotal -= budget;
    }

    void onClear(size_t budget) {
        budgetTotal -= budget;
    }

    static size_t keyTotal;
    static size_t valueTotal;
    static size_t budgetTotal;
};
size_t NonTrivialInsertionEvictionListener::keyTotal;
size_t NonTrivialInsertionEvictionListener::valueTotal;
size_t NonTrivialInsertionEvictionListener::budgetTotal;

template <typename Key, typename Value, typename Estimator, typename Listener>
void assertInKVStore(LRUKeyValue<Key, Value, Estimator, Listener>& cache, Key key, Value value) {
    ASSERT_TRUE(cache.hasKey(key));
    auto s = cache.get(key);
    ASSERT(s.isOK());
    auto kvItr = s.getValue();

    ASSERT_EQUALS(*(kvItr->second), *value);
}

template <typename Key, typename Value, typename Estimator, typename Listener>
void assertNotInKVStore(LRUKeyValue<Key, Value, Estimator, Listener>& cache, Key key) {
    ASSERT_FALSE(cache.hasKey(key));
    auto s = cache.get(key);
    ASSERT(!s.isOK());
}

/**
 * Test that we can add an entry and get it back out.
 */
TEST(LRUKeyValueTest, BasicAddGet) {
    TestSharedPtrValue cache{100};
    auto val = std::make_shared<int>(2);
    cache.add(1, val);
    assertInKVStore(cache, 1, val);
}

/**
 * A kv-store with a max size of 0 isn't too useful, but test
 * that at the very least we don't blow up.
 */
TEST(LRUKeyValueTest, SizeZeroCache) {
    TestSharedPtrValue cache{0};
    cache.add(1, std::make_shared<int>(2));
    assertNotInKVStore(cache, 1);
}

/**
 * Make sure eviction and promotion work properly with a kv-store of size 1.
 */
TEST(LRUKeyValueTest, SizeOneCache) {
    TestSharedPtrValue cache{1};
    auto val = std::make_shared<int>(0);
    cache.add(0, val);
    assertInKVStore(cache, 0, val);

    val = std::make_shared<int>(1);
    // Second entry should immediately evict the first.
    cache.add(1, val);
    assertNotInKVStore(cache, 0);
    assertInKVStore(cache, 1, val);
}

/**
 * Fill up a size 10 kv-store with 10 entries. Call get()
 * on every entry except for one. Then call add() and
 * make sure that the proper entry got evicted.
 */
TEST(LRUKeyValueTest, EvictionTest) {
    int maxSize = 10;
    TestSharedPtrValue cache{static_cast<size_t>(maxSize)};
    for (int i = 0; i < maxSize; ++i) {
        auto nEvicted = cache.add(i, std::make_shared<int>(i));
        ASSERT_EQ(0, nEvicted);
    }
    ASSERT_EQUALS(cache.size(), static_cast<size_t>(maxSize));

    // Call get() on all but one key.
    int evictKey = 5;
    for (int i = 0; i < maxSize; ++i) {
        if (i == evictKey) {
            continue;
        }
        assertInKVStore(cache, i, std::make_shared<int>(i));
    }

    // Adding another entry causes an eviction.
    auto nEvicted = cache.add(maxSize + 1, std::make_shared<int>(maxSize + 1));
    ASSERT_EQUALS(cache.size(), static_cast<size_t>(maxSize));
    ASSERT_EQ(1ul, nEvicted);

    // Check that the least recently accessed has been evicted.
    for (int i = 0; i < maxSize; ++i) {
        if (i == evictKey) {
            assertNotInKVStore(cache, evictKey);
        } else {
            assertInKVStore(cache, i, std::make_shared<int>(i));
        }
    }
}

/**
 * Eviction test with non-trivial budget estimator.
 */
TEST(LRUKeyValueTest, EvictionTestWithNonTrivialEstimator) {
    constexpr size_t maxSize = 55;
    NonTrivialTestSharedPtrValue cache{maxSize};
    size_t item = 0;
    // Adding entries {0, 1}, {1, 2} ... {9, 10} to the LRU store.
    for (; cache.size() + (item + 1) <= maxSize; ++item) {
        auto nEvicted = cache.add(item, std::make_shared<NonTrivialEntry>(item, item + 1));
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

    auto nEvicted =
        cache.add(newItemKey, std::make_shared<NonTrivialEntry>(newItemKey, newItemSize));

    ASSERT_EQ(expectedToBeEvicted, nEvicted);
    ASSERT_EQ(currentSize, cache.size());
}

TEST(LRUKeyValueTest, AddAnEntryWithBiggerThanBudgetSize) {
    constexpr size_t maxSize = 55;
    NonTrivialTestSharedPtrValue cache{maxSize};
    size_t item = 0;
    for (; cache.size() + (item + 1) <= maxSize; ++item) {
        auto nEvicted = cache.add(item, std::make_shared<NonTrivialEntry>(item, item + 1));
        ASSERT_EQ(0, nEvicted);
    }
    ASSERT_EQ(10u, item);
    ASSERT_EQ(maxSize, cache.size());

    auto nEvicted = cache.add(17, std::make_shared<NonTrivialEntry>(15, 57));
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
    TestSharedPtrValue cache{static_cast<size_t>(maxSize)};
    for (int i = 0; i < maxSize; ++i) {
        auto nEvicted = cache.add(i, std::make_shared<int>(i));
        ASSERT_EQ(0, nEvicted);
    }
    ASSERT_EQUALS(cache.size(), static_cast<size_t>(maxSize));

    // Call get() on a particular key.
    int promoteKey = 5;
    assertInKVStore(cache, promoteKey, std::make_shared<int>(promoteKey));

    // Evict all but one of the original entries.
    for (int i = maxSize; i < (maxSize + maxSize - 1); ++i) {
        auto nEvicted = cache.add(i, std::make_shared<int>(i));
        ASSERT_GT(nEvicted, 0);
    }
    ASSERT_EQUALS(cache.size(), static_cast<size_t>(maxSize));

    // Check that the promoteKey has not been evicted.
    for (int i = 0; i < maxSize; ++i) {
        if (i == promoteKey) {
            assertInKVStore(cache, promoteKey, std::make_shared<int>(promoteKey));
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
    TestSharedPtrValue cache{10};
    auto val = std::make_shared<int>(4);
    cache.add(4, val);
    assertInKVStore(cache, 4, val);

    auto newVal = std::make_shared<int>(5);
    cache.add(4, newVal);
    assertInKVStore(cache, 4, newVal);
}

/**
 * Test iteration over the kv-store.
 */
TEST(LRUKeyValueTest, IterationTest) {
    TestSharedPtrValue cache{2};
    cache.add(1, std::make_shared<int>(1));
    cache.add(2, std::make_shared<int>(2));

    auto i = cache.begin();
    ASSERT_EQUALS(i->first, 2);
    ASSERT_EQUALS(*i->second, 2);
    ++i;
    ASSERT_EQUALS(i->first, 1);
    ASSERT_EQUALS(*i->second, 1);
    ++i;
    ASSERT(i == cache.end());
}

TEST(LRUKeyValueTest, RemoveIfTest) {
    TestSharedPtrValue cache{10};
    for (int i = 0; i < 10; ++i) {
        cache.add(i, std::make_shared<int>(i));
    }

    size_t sizeBefore = cache.size();

    // Remove all even keys and "key: 5"
    size_t nRemoved = cache.removeIf([](int key, int entry) { return key % 2 == 0 || entry == 5; });
    ASSERT_EQ(6, nRemoved);

    // Assert that all odd keys are in store execept for "key: 5".
    for (int i = 1; i < 10; i += 2) {
        if (i == 5) {
            assertNotInKVStore(cache, i);
            continue;
        }
        assertInKVStore(cache, i, std::make_shared<int>(i));
    }

    // Assert that all even keys are not in store.
    for (int i = 0; i < 10; i += 2) {
        assertNotInKVStore(cache, i);
    }

    size_t sizeAfter = cache.size();

    ASSERT_EQ(sizeAfter + nRemoved * TrivialBudgetEstimator::kSize, sizeBefore);
}

using TestUniquePtrValue = LRUKeyValue<int, std::unique_ptr<int>, TrivialBudgetEstimator>;

TEST(LRUKeyValueTest, UniquePtrKeyValue) {
    TestUniquePtrValue cache{100};
    cache.add(1, std::make_unique<int>(2));
    assertInKVStore(cache, 1, std::make_unique<int>(2));
    assertNotInKVStore(cache, 3);

    cache.add(1, std::make_unique<int>(3));
    assertInKVStore(cache, 1, std::make_unique<int>(3));

    // Test eviction.
    TestUniquePtrValue cacheForEviction{2};
    cacheForEviction.add(1, std::make_unique<int>(1));
    cacheForEviction.add(2, std::make_unique<int>(2));
    cacheForEviction.add(3, std::make_unique<int>(3));

    ASSERT_EQUALS(cacheForEviction.size(), static_cast<size_t>(2));
    assertNotInKVStore(cacheForEviction, 1);  // The entry with key '1' has been Evicted.
}

using TestScalarValue =
    LRUKeyValue<int, ValueType, TrivialBudgetEstimator, NonTrivialInsertionEvictionListener>;

void assertValueInKVStore(TestScalarValue& cache, int key, ValueType value) {
    ASSERT_TRUE(cache.hasKey(key));
    auto s = cache.get(key);
    ASSERT(s.isOK());
    auto kvItr = s.getValue();

    ASSERT_EQUALS(kvItr->second, value);
}

TEST(LRUKeyValueTest, ScalarKeyValue) {
    TestScalarValue cache{100};
    cache.add(1, ValueType{2});
    assertValueInKVStore(cache, 1, ValueType{2});
    assertNotInKVStore(cache, 3);

    ASSERT_EQUALS(NonTrivialInsertionEvictionListener::keyTotal, 1);
    ASSERT_EQUALS(NonTrivialInsertionEvictionListener::valueTotal, 2);
    ASSERT_EQUALS(NonTrivialInsertionEvictionListener::budgetTotal, 1);

    cache.add(1, ValueType{3});
    assertValueInKVStore(cache, 1, ValueType{3});

    ASSERT_EQUALS(NonTrivialInsertionEvictionListener::keyTotal, 1);
    ASSERT_EQUALS(NonTrivialInsertionEvictionListener::valueTotal, 3);
    ASSERT_EQUALS(NonTrivialInsertionEvictionListener::budgetTotal, 1);

    // Test eviction.
    TestScalarValue cacheForEviction{2};
    cacheForEviction.add(1, ValueType{1});
    cacheForEviction.add(2, ValueType{2});
    cacheForEviction.add(3, ValueType{3});

    ASSERT_EQUALS(cacheForEviction.size(), static_cast<size_t>(2));
    assertNotInKVStore(cacheForEviction, 1);  // The entry with key '1' has been Evicted.

    ASSERT_EQUALS(NonTrivialInsertionEvictionListener::keyTotal, 5);
    ASSERT_EQUALS(NonTrivialInsertionEvictionListener::valueTotal, 5);
    ASSERT_EQUALS(NonTrivialInsertionEvictionListener::budgetTotal, 2);

    // Clear the remaining values.
    cacheForEviction.clear();

    assertNotInKVStore(cacheForEviction, 2);  // The entry with key '2' has been Evicted.
    assertNotInKVStore(cacheForEviction, 3);  // The entry with key '3' has been Evicted.

    ASSERT_EQUALS(NonTrivialInsertionEvictionListener::budgetTotal, 0);
}

}  // namespace
