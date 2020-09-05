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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>
#include <functional>

#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"
#include "mongo/util/invalidating_lru_cache.h"

namespace mongo {
namespace {

// The structure for testing is intentionally made movable, but non-copyable
struct TestValue {
    TestValue(std::string in_value) : value(std::move(in_value)) {}
    TestValue(TestValue&&) = default;

    std::string value;
};

using TestValueCache = InvalidatingLRUCache<int, TestValue>;
using TestValueHandle = TestValueCache::ValueHandle;

using TestValueCacheCausallyConsistent = InvalidatingLRUCache<int, TestValue, Timestamp>;
using TestValueHandleCausallyConsistent = TestValueCacheCausallyConsistent::ValueHandle;

TEST(InvalidatingLRUCacheTest, StandaloneValueHandle) {
    TestValueHandle standaloneHandle({"Standalone value"});
    ASSERT(standaloneHandle.isValid());
    ASSERT_EQ("Standalone value", standaloneHandle->value);
}

TEST(InvalidatingLRUCacheTest, ValueHandleOperators) {
    TestValueCache cache(1);
    cache.insertOrAssign(100, {"Test value"});

    // Test non-const operators
    {
        auto valueHandle = cache.get(100);
        ASSERT_EQ("Test value", valueHandle->value);
        ASSERT_EQ("Test value", (*valueHandle).value);
    }

    // Test const operators
    {
        const auto valueHandle = cache.get(100);
        ASSERT_EQ("Test value", valueHandle->value);
        ASSERT_EQ("Test value", (*valueHandle).value);
    }
}

TEST(InvalidatingLRUCacheTest, CausalConsistency) {
    TestValueCacheCausallyConsistent cache(1);

    cache.insertOrAssign(2, TestValue("Value @ TS 100"), Timestamp(100));
    ASSERT_EQ("Value @ TS 100", cache.get(2, CacheCausalConsistency::kLatestCached)->value);
    ASSERT_EQ("Value @ TS 100", cache.get(2, CacheCausalConsistency::kLatestKnown)->value);

    auto value = cache.get(2, CacheCausalConsistency::kLatestCached);
    cache.advanceTimeInStore(2, Timestamp(200));
    ASSERT_EQ("Value @ TS 100", value->value);
    ASSERT(!value.isValid());
    ASSERT_EQ("Value @ TS 100", cache.get(2, CacheCausalConsistency::kLatestCached)->value);
    ASSERT(!cache.get(2, CacheCausalConsistency::kLatestCached).isValid());
    ASSERT(!cache.get(2, CacheCausalConsistency::kLatestKnown));

    // Intentionally push value for key with a timestamp higher than the one passed to advanceTime
    cache.insertOrAssign(2, TestValue("Value @ TS 300"), Timestamp(300));
    ASSERT_EQ("Value @ TS 100", value->value);
    ASSERT(!value.isValid());
    ASSERT_EQ("Value @ TS 300", cache.get(2, CacheCausalConsistency::kLatestCached)->value);
    ASSERT_EQ("Value @ TS 300", cache.get(2, CacheCausalConsistency::kLatestKnown)->value);
}

TEST(InvalidatingLRUCacheTest, InvalidateNonCheckedOutValue) {
    TestValueCache cache(3);

    cache.insertOrAssign(0, {"Non checked-out (not invalidated)"});
    cache.insertOrAssign(1, {"Non checked-out (invalidated)"});
    cache.insertOrAssign(2, {"Non checked-out (not invalidated)"});

    ASSERT(cache.get(0));
    ASSERT(cache.get(1));
    ASSERT(cache.get(2));
    ASSERT_EQ(3UL, cache.getCacheInfo().size());

    cache.invalidate(1);
    ASSERT(cache.get(0));
    ASSERT(!cache.get(1));
    ASSERT(cache.get(2));
    ASSERT_EQ(2UL, cache.getCacheInfo().size());
}

TEST(InvalidatingLRUCacheTest, InvalidateCheckedOutValue) {
    TestValueCache cache(3);

    cache.insertOrAssign(0, {"Non checked-out (not invalidated)"});
    auto checkedOutValue = cache.insertOrAssignAndGet(1, {"Checked-out (invalidated)"});
    cache.insertOrAssign(2, {"Non checked-out (not invalidated)"});

    ASSERT(checkedOutValue.isValid());
    ASSERT_EQ(3UL, cache.getCacheInfo().size());

    cache.invalidate(1);
    ASSERT(!checkedOutValue.isValid());
    ASSERT_EQ(2UL, cache.getCacheInfo().size());
    ASSERT(cache.get(0));
    ASSERT(!cache.get(1));
    ASSERT(cache.get(2));
}

TEST(InvalidatingLRUCacheTest, InvalidateOneKeyDoesnAffectAnyOther) {
    TestValueCache cache(4);

    auto checkedOutKey = cache.insertOrAssignAndGet(0, {"Checked-out key (0)"});
    auto checkedOutAndInvalidatedKey =
        cache.insertOrAssignAndGet(1, {"Checked-out and invalidated key (1)"});
    cache.insertOrAssign(2, {"Non-checked-out and invalidated key (2)"});
    cache.insertOrAssign(3, {"Key which is not neither checked-out nor invalidated (3)"});

    // Invalidated keys 1 and 2 above and then ensure that only they are affected
    cache.invalidate(1);
    cache.invalidate(2);

    ASSERT(checkedOutKey.isValid());
    ASSERT(!checkedOutAndInvalidatedKey.isValid());
    ASSERT(!cache.get(2));
    ASSERT(cache.get(3));
    ASSERT_EQ(2UL, cache.getCacheInfo().size());
}

TEST(InvalidatingLRUCacheTest, CheckedOutItemsAreInvalidatedWhenEvictedFromCache) {
    TestValueCache cache(3);

    // Make a cache that's at its maximum size
    auto checkedOutKey = cache.insertOrAssignAndGet(
        0, {"Checked-out key which will be discarded and invalidated (0)"});
    (void)cache.insertOrAssignAndGet(1, {"Non-checked-out key (1)"});
    (void)cache.insertOrAssignAndGet(2, {"Non-checked-out key (2)"});

    {
        auto cacheInfo = cache.getCacheInfo();
        std::sort(cacheInfo.begin(), cacheInfo.end(), [](auto a, auto b) { return a.key < b.key; });
        ASSERT_EQ(3UL, cacheInfo.size());
        ASSERT_EQ(0, cacheInfo[0].key);
        ASSERT_EQ(1, cacheInfo[0].useCount);
        ASSERT_EQ(1, cacheInfo[1].key);
        ASSERT_EQ(2, cacheInfo[2].key);
    }

    // Inserting one more key, should boot out key 0, which was inserted first, but it should still
    // show-up in the statistics for the cache
    cache.insertOrAssign(3, {"Non-checked-out key (3)"});

    {
        auto cacheInfo = cache.getCacheInfo();
        std::sort(cacheInfo.begin(), cacheInfo.end(), [](auto a, auto b) { return a.key < b.key; });
        ASSERT_EQ(4UL, cacheInfo.size());
        ASSERT_EQ(0, cacheInfo[0].key);
        ASSERT_EQ(1, cacheInfo[0].useCount);
        ASSERT_EQ(1, cacheInfo[1].key);
        ASSERT_EQ(2, cacheInfo[2].key);
        ASSERT_EQ(3, cacheInfo[3].key);
    }

    // Invalidating the key, which was booted out due to cache size exceeded should still be
    // reflected on the checked-out key
    ASSERT(checkedOutKey.isValid());
    cache.invalidate(0);
    ASSERT(!checkedOutKey.isValid());

    {
        auto cacheInfo = cache.getCacheInfo();
        std::sort(cacheInfo.begin(), cacheInfo.end(), [](auto a, auto b) { return a.key < b.key; });
        ASSERT_EQ(3UL, cacheInfo.size());
        ASSERT_EQ(1, cacheInfo[0].key);
        ASSERT_EQ(2, cacheInfo[1].key);
        ASSERT_EQ(3, cacheInfo[2].key);
    }
}

TEST(InvalidatingLRUCacheTest, CheckedOutItemsAreInvalidatedWithPredicateWhenEvictedFromCache) {
    TestValueCache cache(3);

    // Make a cache that's at its maximum size
    auto checkedOutKey = cache.insertOrAssignAndGet(
        0, {"Checked-out key which will be discarded and invalidated (0)"});
    (void)cache.insertOrAssignAndGet(1, {"Non-checked-out key (1)"});
    (void)cache.insertOrAssignAndGet(2, {"Non-checked-out key (2)"});

    {
        auto cacheInfo = cache.getCacheInfo();
        std::sort(cacheInfo.begin(), cacheInfo.end(), [](auto a, auto b) { return a.key < b.key; });
        ASSERT_EQ(3UL, cacheInfo.size());
        ASSERT_EQ(0, cacheInfo[0].key);
        ASSERT_EQ(1, cacheInfo[0].useCount);
        ASSERT_EQ(1, cacheInfo[1].key);
        ASSERT_EQ(2, cacheInfo[2].key);
    }

    // Inserting one more key, should boot out key 0, which was inserted first, but it should still
    // show-up in the statistics for the cache
    cache.insertOrAssign(3, {"Non-checked-out key (3)"});

    {
        auto cacheInfo = cache.getCacheInfo();
        std::sort(cacheInfo.begin(), cacheInfo.end(), [](auto a, auto b) { return a.key < b.key; });
        ASSERT_EQ(4UL, cacheInfo.size());
        ASSERT_EQ(0, cacheInfo[0].key);
        ASSERT_EQ(1, cacheInfo[0].useCount);
        ASSERT_EQ(1, cacheInfo[1].key);
        ASSERT_EQ(2, cacheInfo[2].key);
        ASSERT_EQ(3, cacheInfo[3].key);
    }

    // Invalidating the key, which was booted out due to cache size exceeded should still be
    // reflected on the checked-out key
    ASSERT(checkedOutKey.isValid());
    cache.invalidateIf([](int key, TestValue* value) { return key == 0; });
    ASSERT(!checkedOutKey.isValid());

    {
        auto cacheInfo = cache.getCacheInfo();
        std::sort(cacheInfo.begin(), cacheInfo.end(), [](auto a, auto b) { return a.key < b.key; });
        ASSERT_EQ(3UL, cacheInfo.size());
        ASSERT_EQ(1, cacheInfo[0].key);
        ASSERT_EQ(2, cacheInfo[1].key);
        ASSERT_EQ(3, cacheInfo[2].key);
    }
}

TEST(InvalidatingLRUCacheTest, CausalConsistencyPreservedForEvictedCheckedOutKeys) {
    TestValueCacheCausallyConsistent cache(1);

    auto key1ValueAtTS10 =
        cache.insertOrAssignAndGet(1, TestValue("Key 1 - Value @ TS 10"), Timestamp(10));

    // This will evict key 1, but we have a handle to it, so it will stay accessible on the evicted
    // list
    cache.insertOrAssign(2, TestValue("Key 2 - Value @ TS 20"), Timestamp(20));

    auto [cachedValueAtTS10, timeInStoreAtTS10] = cache.getCachedValueAndTimeInStore(1);
    ASSERT_EQ(Timestamp(10), timeInStoreAtTS10);
    ASSERT_EQ("Key 1 - Value @ TS 10", cachedValueAtTS10->value);
    ASSERT_EQ("Key 1 - Value @ TS 10", key1ValueAtTS10->value);
    ASSERT_EQ("Key 1 - Value @ TS 10", cache.get(1, CacheCausalConsistency::kLatestCached)->value);
    ASSERT_EQ("Key 1 - Value @ TS 10", cache.get(1, CacheCausalConsistency::kLatestKnown)->value);

    cache.advanceTimeInStore(1, Timestamp(11));
    auto [cachedValueAtTS11, timeInStoreAtTS11] = cache.getCachedValueAndTimeInStore(1);
    ASSERT_EQ(Timestamp(11), timeInStoreAtTS11);
    ASSERT(!key1ValueAtTS10.isValid());
    ASSERT_EQ("Key 1 - Value @ TS 10", cachedValueAtTS11->value);
    ASSERT_EQ("Key 1 - Value @ TS 10", key1ValueAtTS10->value);
    ASSERT_EQ("Key 1 - Value @ TS 10", cache.get(1, CacheCausalConsistency::kLatestCached)->value);
    ASSERT(!cache.get(1, CacheCausalConsistency::kLatestKnown));

    cache.insertOrAssign(1, TestValue("Key 1 - Value @ TS 12"), Timestamp(12));
    ASSERT_EQ("Key 1 - Value @ TS 12", cache.get(1, CacheCausalConsistency::kLatestCached)->value);
    ASSERT_EQ("Key 1 - Value @ TS 12", cache.get(1, CacheCausalConsistency::kLatestKnown)->value);
}

TEST(InvalidatingLRUCacheTest, InvalidateAfterAdvanceTime) {
    TestValueCacheCausallyConsistent cache(1);

    cache.insertOrAssign(20, TestValue("Value @ TS 200"), Timestamp(200));
    cache.advanceTimeInStore(20, Timestamp(250));
    ASSERT_EQ("Value @ TS 200", cache.get(20, CacheCausalConsistency::kLatestCached)->value);
    ASSERT(!cache.get(20, CacheCausalConsistency::kLatestKnown));

    cache.invalidate(20);
    ASSERT(!cache.get(20, CacheCausalConsistency::kLatestCached));
    ASSERT(!cache.get(20, CacheCausalConsistency::kLatestKnown));
}

TEST(InvalidatingLRUCacheTest, InsertEntryAtTimeLessThanAdvanceTime) {
    TestValueCacheCausallyConsistent cache(1);

    cache.insertOrAssign(20, TestValue("Value @ TS 200"), Timestamp(200));
    cache.advanceTimeInStore(20, Timestamp(300));
    ASSERT_EQ("Value @ TS 200", cache.get(20, CacheCausalConsistency::kLatestCached)->value);
    ASSERT(!cache.get(20, CacheCausalConsistency::kLatestKnown));

    cache.insertOrAssign(20, TestValue("Value @ TS 250"), Timestamp(250));
    ASSERT_EQ("Value @ TS 250", cache.get(20, CacheCausalConsistency::kLatestCached)->value);
    ASSERT(!cache.get(20, CacheCausalConsistency::kLatestKnown));

    cache.insertOrAssign(20, TestValue("Value @ TS 300"), Timestamp(300));
    ASSERT_EQ("Value @ TS 300", cache.get(20, CacheCausalConsistency::kLatestCached)->value);
    ASSERT_EQ("Value @ TS 300", cache.get(20, CacheCausalConsistency::kLatestKnown)->value);
}

TEST(InvalidatingLRUCacheTest, OrderOfDestructionOfHandlesDiffersFromOrderOfInsertion) {
    TestValueCache cache(1);

    boost::optional<TestValueCache::ValueHandle> firstValue(
        cache.insertOrAssignAndGet(100, {"Key 100, Value 1"}));
    ASSERT(*firstValue);
    ASSERT(firstValue->isValid());

    // This will invalidate the first value of key 100
    auto secondValue = cache.insertOrAssignAndGet(100, {"Key 100, Value 2"});
    ASSERT(secondValue);
    ASSERT(secondValue.isValid());
    ASSERT(!firstValue->isValid());

    // This will evict the second value of key 100
    cache.insertOrAssignAndGet(200, {"Key 200, Value 1"});
    ASSERT(secondValue);
    ASSERT(secondValue.isValid());
    ASSERT(!firstValue->isValid());

    // This makes the first value of 100's handle go away before the second value's hande
    firstValue.reset();
    ASSERT(secondValue.isValid());

    cache.invalidate(100);
    ASSERT(!secondValue.isValid());
}

TEST(InvalidatingLRUCacheTest, AssignWhileValueIsCheckedOutInvalidatesFirstValue) {
    TestValueCache cache(1);

    auto firstGet = cache.insertOrAssignAndGet(100, {"First check-out of the key (100)"});
    ASSERT(firstGet);
    ASSERT(firstGet.isValid());

    auto secondGet = cache.insertOrAssignAndGet(100, {"Second check-out of the key (100)"});
    ASSERT(!firstGet.isValid());
    ASSERT(secondGet);
    ASSERT(secondGet.isValid());
    ASSERT_EQ("Second check-out of the key (100)", secondGet->value);
}

TEST(InvalidatingLRUCacheTest, InvalidateIfAllEntries) {
    TestValueCache cache(6);

    for (int i = 0; i < 6; i++) {
        cache.insertOrAssign(i, TestValue{str::stream() << "Test value " << i});
    }

    const std::vector checkedOutValues{cache.get(0), cache.get(1), cache.get(2)};
    cache.invalidateIf([](int, TestValue*) { return true; });
}

TEST(InvalidatingLRUCacheTest, CacheSizeZero) {
    TestValueCache cache(0);

    {
        auto immediatelyEvictedValue = cache.insertOrAssignAndGet(0, TestValue{"Evicted value"});
        auto anotherImmediatelyEvictedValue =
            cache.insertOrAssignAndGet(1, TestValue{"Another evicted value"});
        ASSERT(immediatelyEvictedValue.isValid());
        ASSERT(anotherImmediatelyEvictedValue.isValid());
        ASSERT(cache.get(0));
        ASSERT(cache.get(1));
        ASSERT_EQ(2UL, cache.getCacheInfo().size());
        ASSERT_EQ(1UL, cache.getCacheInfo()[0].useCount);
        ASSERT_EQ(1UL, cache.getCacheInfo()[1].useCount);
    }

    ASSERT(!cache.get(0));
    ASSERT(!cache.get(1));
    ASSERT_EQ(0UL, cache.getCacheInfo().size());
}

TEST(InvalidatingLRUCacheTest, CacheSizeZeroInvalidate) {
    TestValueCache cache(0);

    auto immediatelyEvictedValue = cache.insertOrAssignAndGet(0, TestValue{"Evicted value"});
    ASSERT(immediatelyEvictedValue.isValid());
    ASSERT(cache.get(0));
    ASSERT_EQ(1UL, cache.getCacheInfo().size());
    ASSERT_EQ(1UL, cache.getCacheInfo()[0].useCount);

    cache.invalidate(0);

    ASSERT(!immediatelyEvictedValue.isValid());
    ASSERT(!cache.get(0));
    ASSERT_EQ(0UL, cache.getCacheInfo().size());
}

TEST(InvalidatingLRUCacheTest, CacheSizeZeroInvalidateAllEntries) {
    TestValueCache cache(0);

    std::vector<TestValueHandle> checkedOutValues;
    for (int i = 0; i < 6; i++) {
        checkedOutValues.emplace_back(
            cache.insertOrAssignAndGet(i, TestValue{str::stream() << "Test value " << i}));
    }

    cache.invalidateIf([](int, TestValue*) { return true; });

    for (auto&& value : checkedOutValues) {
        ASSERT(!value.isValid()) << "Value " << value->value << " was not invalidated";
    }
}

TEST(InvalidatingLRUCacheTest, CacheSizeZeroCausalConsistency) {
    TestValueCacheCausallyConsistent cache(0);

    cache.advanceTimeInStore(100, Timestamp(30));
    cache.insertOrAssign(100, TestValue("Value @ TS 30"), Timestamp(30));
    auto [cachedValueAtTS30, timeInStoreAtTS30] = cache.getCachedValueAndTimeInStore(100);
    ASSERT_EQ(Timestamp(), timeInStoreAtTS30);
    ASSERT(!cachedValueAtTS30);

    auto valueAtTS30 = cache.insertOrAssignAndGet(100, TestValue("Value @ TS 30"), Timestamp(30));
    ASSERT_EQ("Value @ TS 30", cache.get(100, CacheCausalConsistency::kLatestCached)->value);
    ASSERT_EQ("Value @ TS 30", cache.get(100, CacheCausalConsistency::kLatestKnown)->value);

    cache.advanceTimeInStore(100, Timestamp(35));
    auto [cachedValueAtTS35, timeInStoreAtTS35] = cache.getCachedValueAndTimeInStore(100);
    ASSERT_EQ(Timestamp(35), timeInStoreAtTS35);
    ASSERT_EQ("Value @ TS 30", cachedValueAtTS35->value);
    ASSERT_EQ("Value @ TS 30", cache.get(100, CacheCausalConsistency::kLatestCached)->value);
    ASSERT(!cache.get(100, CacheCausalConsistency::kLatestKnown));

    auto valueAtTS40 = cache.insertOrAssignAndGet(100, TestValue("Value @ TS 40"), Timestamp(40));
    ASSERT_EQ("Value @ TS 40", cache.get(100, CacheCausalConsistency::kLatestCached)->value);
    ASSERT_EQ("Value @ TS 40", cache.get(100, CacheCausalConsistency::kLatestKnown)->value);
}

template <class TCache, typename TestFunc>
void parallelTest(size_t cacheSize, TestFunc doTest) {
    constexpr auto kNumIterations = 100'000;
    constexpr auto kNumThreads = 4;

    TCache cache(cacheSize);

    std::vector<stdx::thread> threads;
    for (int i = 0; i < kNumThreads; i++) {
        threads.emplace_back([&] {
            for (int i = 0; i < kNumIterations / kNumThreads; i++) {
                doTest(cache);
            }
        });
    }

    for (auto&& thread : threads) {
        thread.join();
    }
}

TEST(InvalidatingLRUCacheParallelTest, InsertOrAssignThenGet) {
    parallelTest<TestValueCache>(1, [](auto& cache) {
        const int key = 100;
        cache.insertOrAssign(key, TestValue{"Parallel tester value"});

        auto cachedItem = cache.get(key);
        if (!cachedItem || !cachedItem.isValid())
            return;

        auto cachedItemSecondRef = cachedItem;
        cache.invalidate(key);

        ASSERT(!cachedItem.isValid());
        ASSERT(!cachedItemSecondRef.isValid());
    });
}

TEST(InvalidatingLRUCacheParallelTest, InsertOrAssignAndGet) {
    parallelTest<TestValueCache>(1, [](auto& cache) {
        const int key = 200;
        auto cachedItem = cache.insertOrAssignAndGet(key, TestValue{"Parallel tester value"});
        ASSERT(cachedItem);

        auto cachedItemSecondRef = cache.get(key);
        ASSERT(cachedItemSecondRef);
    });
}

TEST(InvalidatingLRUCacheParallelTest, CacheSizeZeroInsertOrAssignAndGet) {
    parallelTest<TestValueCache>(0, [](auto& cache) {
        const int key = 300;
        auto cachedItem = cache.insertOrAssignAndGet(key, TestValue{"Parallel tester value"});
        ASSERT(cachedItem);

        auto cachedItemSecondRef = cache.get(key);
    });
}

TEST(InvalidatingLRUCacheParallelTest, AdvanceTime) {
    AtomicWord<uint64_t> counter{1};
    Mutex insertOrAssignMutex = MONGO_MAKE_LATCH("ReadThroughCacheBase::_cancelTokenMutex");

    parallelTest<TestValueCacheCausallyConsistent>(0, [&](auto& cache) {
        const int key = 300;
        {
            // The calls to insertOrAssign must always pass strictly incrementing time
            stdx::lock_guard lg(insertOrAssignMutex);
            cache.insertOrAssign(
                key, TestValue{"Parallel tester value"}, Timestamp(counter.fetchAndAdd(1)));
        }

        auto latestCached = cache.get(key, CacheCausalConsistency::kLatestCached);
        auto latestKnown = cache.get(key, CacheCausalConsistency::kLatestKnown);

        cache.advanceTimeInStore(key, Timestamp(counter.fetchAndAdd(1)));
    });
}

}  // namespace
}  // namespace mongo
