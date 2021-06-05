/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/stdx/unordered_map.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <string>

#include <fmt/format.h>

#include "mongo/unittest/unittest.h"

namespace {

using namespace fmt::literals;

template <typename Map>
std::string dumpMap(const Map& m) {
    std::string r;
    r += "{";
    const char* comma = "";
    for (auto&& [k, v] : m) {
        r += "{}{}:{}"_format(comma, k, v);
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
    auto pred = [](auto&& e) { return e.second < 0; };
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
