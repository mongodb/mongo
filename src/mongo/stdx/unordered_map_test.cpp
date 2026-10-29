// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/stdx/unordered_map.h"

#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <stdexcept>
#include <string>

#include <absl/container/node_hash_map.h>
#include <fmt/format.h>

namespace {

template <typename Map>
std::string dumpMap(const Map& m) {
    std::string r;
    r += "{";
    const char* comma = "";
    for (auto&& [k, v] : m) {
        r += fmt::format("{}{}:{}", comma, k, v);
        comma = ", ";
    }
    r += "}";
    return r;
}

TEST(StdxUnorderedMapTest, atShouldThrow) {
    mongo::stdx::unordered_map<int, int> m;
    ASSERT_THROWS(m.at(42), std::out_of_range);
}

TEST(StdxUnorderedMapTest, EraseIf) {
    // Eliminate negative-valued elements from a copy of `pre`.
    const mongo::stdx::unordered_map<int, int> pre{
        {100, 0},
        {101, -1},
        {102, 2},
        {103, -3},
        {104, -4},
        {105, 5},
        {106, 6},
        {107, 7},
    };
    auto pred = [](auto&& e) {
        return e.second < 0;
    };
    size_t predCount = std::count_if(pre.begin(), pre.end(), pred);

    auto map = pre;
    ASSERT_EQ(mongo::stdx::erase_if(map, pred), predCount);
    ASSERT_EQ(map.size(), pre.size() - predCount) << dumpMap(map);
    for (auto&& e : pre) {
        bool inMap = std::find(map.begin(), map.end(), e) != map.end();
        ASSERT_EQ(inMap, !pred(e)) << dumpMap(map);
    }
}
}  // namespace
