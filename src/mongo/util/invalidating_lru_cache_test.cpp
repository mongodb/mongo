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
    TestValue(const TestValue&) = delete;
    TestValue& operator=(const TestValue&) = delete;

    std::string value;
};

using TestValueCache = InvalidatingLRUCache<int, TestValue>;
using TestValueHandle = TestValueCache::ValueHandle;

TEST(InvalidatingLRUCacheTest, ValueHandleOperators) {
    TestValueCache cache(1);
    cache.insertOrAssign(100, {"Test value"});

    {
        auto valueHandle = cache.get(100);
        ASSERT_EQ("Test value", valueHandle->value);
        ASSERT_EQ("Test value", (*valueHandle).value);
    }
    {
        const auto valueHandle = cache.get(100);
        ASSERT_EQ("Test value", valueHandle->value);
        ASSERT_EQ("Test value", (*valueHandle).value);
    }
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

template <typename TestFunc>
void parallelTest(size_t cacheSize, TestFunc doTest) {
    constexpr auto kNumIterations = 100'000;
    constexpr auto kNumThreads = 4;

    TestValueCache cache(cacheSize);

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
    parallelTest(1, [](TestValueCache& cache) mutable {
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
    parallelTest(1, [](auto& cache) {
        const int key = 200;
        auto cachedItem = cache.insertOrAssignAndGet(key, TestValue{"Parallel tester value"});
        ASSERT(cachedItem);

        auto cachedItemSecondRef = cache.get(key);
        ASSERT(cachedItemSecondRef);
    });
}

TEST(InvalidatingLRUCacheParallelTest, CacheSizeZeroInsertOrAssignAndGet) {
    parallelTest(0, [](TestValueCache& cache) mutable {
        const int key = 300;
        auto cachedItem = cache.insertOrAssignAndGet(key, TestValue{"Parallel tester value"});
        ASSERT(cachedItem);

        auto cachedItemSecondRef = cache.get(key);
    });
}

}  // namespace
}  // namespace mongo
