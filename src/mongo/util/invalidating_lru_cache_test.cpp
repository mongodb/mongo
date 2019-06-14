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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

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

class TestValue {
public:
    bool isValid() {
        return _isValidHook.value_or([&] { return _isValid.load(); })();
    }

    void markValid(bool isValid) {
        _isValid.store(isValid);
    }

    void setValidHook(std::function<bool()> validHook) {
        _isValidHook = std::move(validHook);
    }

private:
    AtomicWord<bool> _isValid{true};
    boost::optional<std::function<bool()>> _isValidHook;
};

struct TestValueInvalidator {
    void operator()(TestValue* value) {
        value->markValid(false);
    }
};

TEST(InvalidatingLRUCache, Invalidate) {
    constexpr int key = 0;
    InvalidatingLRUCache<int, TestValue, TestValueInvalidator> cache(1, TestValueInvalidator{});

    auto validItem = std::make_unique<TestValue>();
    const auto validItemPtr = validItem.get();
    // Insert an item into the cache.
    cache.insertOrAssign(0, std::move(validItem));

    // Make sure the cache now contains 1 cached item that mainsertOrAssign(0,
    // std::move(validItem));
    auto cacheInfo = cache.getCacheInfo();
    ASSERT_EQ(cacheInfo.size(), size_t(1));
    ASSERT_FALSE(cacheInfo[0].active);
    ASSERT_EQ(cacheInfo[0].key, key);

    // Get the cached item out as a shared_ptr and verify that it's valid and matches the item
    // we just inserted - both by key and by pointer.
    auto cachedItem = cache.get(key);
    ASSERT_TRUE(cachedItem);
    ASSERT_EQ(cachedItem->get(), validItemPtr);
    ASSERT_TRUE((*cachedItem));

    // Make sure the cache info reflects that the cache size is 1, but the item is now active.
    cacheInfo = cache.getCacheInfo();
    ASSERT_EQ(cacheInfo.size(), size_t(1));
    ASSERT_TRUE(cacheInfo[0].active);
    ASSERT_EQ(cacheInfo[0].key, key);

    // Invalidate the active item and make sure the invalidator ran and that we can still access
    // our shared_ptr to the item.
    cache.invalidate(key);
    ASSERT_FALSE((*cachedItem)->isValid());

    // The cache should now be totally empty.
    cacheInfo = cache.getCacheInfo();
    ASSERT_TRUE(cacheInfo.empty());

    // Insert a new item into the cache at the same key.
    cache.insertOrAssign(key, std::make_unique<TestValue>());

    // Make sure we can get the new item from the cache. By assigning to cachedItem, we should
    // destroy the old shared_ptr as well.
    cachedItem = cache.get(key);
    ASSERT_TRUE(cachedItem);

    // Make sure the new item isn't just the old item over again.
    ASSERT_NE(cachedItem->get(), validItemPtr);
    cacheInfo = cache.getCacheInfo();
    ASSERT_EQ(cacheInfo.size(), size_t(1));
}

TEST(InvalidatingLRUCache, CacheFull) {
    constexpr int cacheSize = 2;
    InvalidatingLRUCache<int, TestValue, TestValueInvalidator> cache(cacheSize,
                                                                     TestValueInvalidator{});
    // Make a cache that's absolutely full of items.
    for (int i = 0; i < cacheSize; i++) {
        cache.insertOrAssign(i, std::make_unique<TestValue>());
    }

    auto cacheInfo = cache.getCacheInfo();
    ASSERT_EQ(cacheInfo.size(), static_cast<size_t>(cacheSize));

    // Make sure we can actually get an item. This should move item 0 from the LRU cache and into
    // the active list.
    auto zeroItem = cache.get(0);
    ASSERT_TRUE(zeroItem);

    // Insert a new item into the cache. Because item zero is in the active list, the cache info
    // should have 4 items with one of them active.
    cache.insertOrAssign(size_t(cacheSize + 1), std::make_unique<TestValue>());
    cacheInfo = cache.getCacheInfo();
    ASSERT_EQ(cacheInfo.size(), size_t(cacheSize + 1));

    auto zeroInfoIt = std::find_if(
        cacheInfo.begin(), cacheInfo.end(), [](const auto& info) { return info.key == 0; });
    ASSERT_TRUE(zeroInfoIt != cacheInfo.end());
    ASSERT_TRUE(zeroInfoIt->active);
    ASSERT_EQ(zeroInfoIt->useCount, 1);

    // release our active item by assigning it to boost::none. This should bump item 1 out of the
    // cache because it was the least recently used item.
    zeroItem = boost::none;
    cacheInfo = cache.getCacheInfo();
    ASSERT_EQ(cacheInfo.size(), size_t(cacheSize));

    auto oneItem = cache.get(1);
    ASSERT_FALSE(oneItem);
}

TEST(InvalidatingLRUCache, InvalidateIf) {
    constexpr int cacheSize = 3;
    InvalidatingLRUCache<int, TestValue, TestValueInvalidator> cache(cacheSize,
                                                                     TestValueInvalidator{});

    for (int i = 0; i < cacheSize; i++) {
        cache.insertOrAssign(i, std::make_unique<TestValue>());
    }

    constexpr int middleItem = 1;
    auto middle = cache.get(middleItem);
    ASSERT_TRUE(middle);
    auto middleVal = std::move(*middle);

    auto cacheInfo = cache.getCacheInfo();
    auto infoIt = std::find_if(cacheInfo.begin(), cacheInfo.end(), [&](const auto& info) {
        return info.key == middleItem;
    });
    ASSERT_EQ(cacheInfo.size(), static_cast<size_t>(cacheSize));
    ASSERT_TRUE(infoIt->active);

    cache.invalidateIf([&](const int& key, const TestValue*) { return (key == middleItem); });

    ASSERT_FALSE(middleVal->isValid());
    cacheInfo = cache.getCacheInfo();
    ASSERT_EQ(cacheInfo.size(), static_cast<size_t>(cacheSize) - 1);
}

}  // namespace
}  // namespace mongo
