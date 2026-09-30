/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <string>

#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrent_shared_values_map.h"

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
    ConcurrentSharedValuesMap<int, AtomicWord<int>> map;

    map.getOrEmplace(1, 1);
    map.getOrEmplace(2, 1);
    map.getOrEmplace(3, 1);

    auto snapshot = map.getUnderlyingSnapshot();
    snapshot->at(1)->addAndFetch(1);

    ASSERT_EQ(map.find(1)->load(), 2);
}
}  // namespace
}  // namespace mongo
