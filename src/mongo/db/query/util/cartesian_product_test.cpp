// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/util/cartesian_product.h"

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
