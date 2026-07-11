// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/util/disjoint_set.h"

#include "mongo/unittest/unittest.h"

#include <unordered_map>
#include <vector>

namespace mongo {
namespace {
std::vector<std::vector<size_t>> extractSets(DisjointSet& ds) {
    std::unordered_map<size_t, std::vector<size_t>> sets;
    for (size_t i = 0; i < ds.size(); ++i) {
        sets[*ds.find(i)].push_back(i);
    }
    std::vector<std::vector<size_t>> result;
    for (const auto& [_, set] : sets) {
        result.emplace_back(std::move(set));
    }
    return result;
}

TEST(DisjointSetTest, Smoke) {
    DisjointSet ds{10};
    ASSERT_EQ(ds.size(), 10);

    ASSERT_NE(ds.find(4), ds.find(7));
    ds.unite(4, 7);  // set: {4, 7}
    ASSERT_EQ(ds.find(4), ds.find(7));
    ASSERT_NE(ds.find(4), ds.find(8));

    ds.unite(1, 9);  // set: {1, 9}
    ASSERT_NE(ds.find(8), ds.find(1));
    ds.unite(9, 8);  //  set: {1, 8, 9}
    ASSERT_EQ(ds.find(8), ds.find(1));

    ASSERT_NE(ds.find(4), ds.find(9));
    ds.unite(8, 7);  // set: {1, 4, 7, 8, 9}
    ASSERT_EQ(ds.find(4), ds.find(9));

    ds.unite(2, 3);  // set: {2, 3}
    ds.unite(5, 6);  // set: {5, 6}
    ds.unite(3, 5);  // set: {2, 3, 5, 6}

    // At this moment the following sets are expected: {0}, {2, 3, 5, 6}, {1, 4, 7, 8, 9}
    auto sets = extractSets(ds);

    ASSERT_EQ(sets.size(), 3);
    for (const auto& set : sets) {
        switch (set.size()) {
            case 1:
                ASSERT_EQ(set.front(), 0);
                break;
            case 4:
                ASSERT_EQ(set, std::vector<size_t>({2, 3, 5, 6}));
                break;
            case 5:
                ASSERT_EQ(set, std::vector<size_t>({1, 4, 7, 8, 9}));
                break;
            default:
                FAIL("Unexpected set");
        }
    }

    ds.clear();
    ASSERT_NE(ds.find(4), ds.find(7));
    ASSERT_NE(ds.find(8), ds.find(1));
    ASSERT_NE(ds.find(4), ds.find(9));
}

TEST(DisjointSetTest, OutOfBound) {
    DisjointSet ds{10};

    // No root for out of range element.
    ASSERT_EQ(ds.find(100), boost::none);

    // This is no opt to combine two out of range elements.
    ds.unite(100, 200);
}
}  // namespace
}  // namespace mongo
