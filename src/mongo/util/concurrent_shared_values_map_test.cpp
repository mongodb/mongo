// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/concurrent_shared_values_map.h"

#include "mongo/platform/atomic.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/unittest.h"

#include <string>

#include <absl/container/flat_hash_map.h>

namespace mongo {
namespace {

TEST(ConcurrentSharedValuesMapTest, FindSurvivesLifetime) {
    ConcurrentSharedValuesMap<int, std::string> map;

    map.getOrEmplace(1, "test value");

    auto ref1 = map.find(1);
    ASSERT_TRUE(ref1);
    ASSERT_EQ(*ref1, "test value");

    map.erase(1);
    // Entry doesn't exist anymore
    ASSERT_FALSE(map.find(1));

    // But reference is still valid and works properly
    ASSERT_EQ(*ref1, "test value");
}

TEST(ConcurrentSharedValuesMapTest, CopyOfMapSurvives) {
    ConcurrentSharedValuesMap<int, std::string> map;

    map.getOrEmplace(1, "test value");

    auto ref1 = map.find(1);
    ASSERT_TRUE(ref1);
    ASSERT_EQ(*ref1, "test value");

    // Make a copy of the map
    auto copyOfMap = map;

    map.erase(1);
    ASSERT_FALSE(map.find(1));

    ASSERT_EQ(*ref1, "test value");
    auto otherRef = copyOfMap.find(1);
    // Entry still exists in the map copy and points to the same value
    ASSERT_TRUE(otherRef);
    ASSERT_EQ(otherRef, ref1);
}

TEST(ConcurrentSharedValuesMapTest, GetOrEmplaceInsertsOnce) {
    ConcurrentSharedValuesMap<int, std::string> map;

    auto ref1 = map.getOrEmplace(1, "ok");
    auto ref2 = map.getOrEmplace(1, "not ok");
    ASSERT_EQ(ref1, ref2);
}

TEST(ConcurrentSharedValuesMapTest, ReplaceWithOtherMap) {
    ConcurrentSharedValuesMap<int, std::string> map;

    using Map = decltype(map)::Map;

    auto ref1 = map.getOrEmplace(1, "old value");

    map.updateWith([](const Map& oldMap) {
        Map newResult;
        newResult.emplace(1, std::make_shared<std::string>("new value"));
        return newResult;
    });

    ASSERT_EQ(*ref1, "old value");

    auto ref2 = map.find(1);
    ASSERT_TRUE(ref2);
    ASSERT_EQ(*ref2, "new value");
}

TEST(ConcurrentSharedValuesMapTest, ForEachTest) {
    ConcurrentSharedValuesMap<int, std::string> map;

    map.getOrEmplace(1, "value");
    map.getOrEmplace(2, "value");
    map.getOrEmplace(3, "value");

    stdx::unordered_set<int> pendingToSee = {1, 2, 3};

    for (auto snapshot = map.getUnderlyingSnapshot(); const auto& [key, value] : *snapshot) {
        ASSERT_EQ(pendingToSee.erase(key), 1);
    }

    ASSERT_TRUE(pendingToSee.empty());

    ConcurrentSharedValuesMap<int, std::string> emptyMap;
    for (auto snapshot = emptyMap.getUnderlyingSnapshot(); const auto& unused : *snapshot) {
        // This should not execute
        (void)unused;
        ASSERT_TRUE(false);
    }
}

TEST(ConcurrentSharedValuesMapTest, TestSnapshotCanModifyValues) {
    ConcurrentSharedValuesMap<int, Atomic<int>> map;

    map.getOrEmplace(1, 1);
    map.getOrEmplace(2, 1);
    map.getOrEmplace(3, 1);

    auto snapshot = map.getUnderlyingSnapshot();
    snapshot->at(1)->addAndFetch(1);

    ASSERT_EQ(map.find(1)->load(), 2);
}
}  // namespace
}  // namespace mongo
