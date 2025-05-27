/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "cartesian_product.h"

#include "mongo/unittest/unittest.h"

#include <algorithm>


namespace mongo {

TEST(RangesTest, CartesianProduct) {
    std::array a{'A', 'B', 'C'};
    std::vector b{1};
    std::list<std::string> c{"foo", "bar"};
    auto cartesian = utils::cartesian_product(a, b, c);

    std::array<std::tuple<char, int, std::string>, 6> expected{{
        {'A', 1, "foo"},
        {'A', 1, "bar"},
        {'B', 1, "foo"},
        {'B', 1, "bar"},
        {'C', 1, "foo"},
        {'C', 1, "bar"},
    }};

    ASSERT_TRUE(std::equal(expected.begin(), expected.end(), cartesian.begin(), cartesian.end()));
}

TEST(RangesTest, CartesianProductSingle) {
    std::array a{'A', 'B', 'C'};
    auto cartesian = utils::cartesian_product(a);

    std::array<std::tuple<char>, 3> expected{{'A', 'B', 'C'}};

    ASSERT_TRUE(std::equal(expected.begin(), expected.end(), cartesian.begin(), cartesian.end()));
}

TEST(RangesTest, CartesianProductOneEmpty) {
    std::array a{'A', 'B', 'C'};
    std::vector<int> b{};
    std::list<std::string> c{"foo", "bar"};
    auto cartesian = utils::cartesian_product(a, b, c);

    std::array<std::tuple<char, int, std::string>, 0> expected{};

    ASSERT_TRUE(std::equal(expected.begin(), expected.end(), cartesian.begin(), cartesian.end()));
}

TEST(RangesTest, CartesianProductAllEmpty) {
    std::array<char, 0> a{};
    std::vector<int> b{};
    std::list<std::string> c{};
    auto cartesian = utils::cartesian_product(a, b, c);

    std::array<std::tuple<char, int, std::string>, 0> expected{};

    ASSERT_TRUE(std::equal(expected.begin(), expected.end(), cartesian.begin(), cartesian.end()));
}

TEST(RangesTest, CartesianProductNoBases) {
    auto cartesian = utils::cartesian_product();

    // This is consistent with C++23 cartesian_product; for no inputs, it is expression-equivalent
    // to views::single(std::tuple()) - a single result, which is an empty tuple.
    std::array<std::tuple<>, 1> expected{};

    ASSERT_TRUE(std::equal(expected.begin(), expected.end(), cartesian.begin(), cartesian.end()));
}
}  // namespace mongo
