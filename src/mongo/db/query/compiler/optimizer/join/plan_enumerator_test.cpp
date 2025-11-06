/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/join/plan_enumerator.h"

#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {

TEST(JoinPlanEnumerator, InitializeSubsetsTwo) {
    JoinGraph graph;
    graph.addNode(NamespaceString::createNamespaceString_forTest("a"), nullptr, boost::none);
    graph.addNode(NamespaceString::createNamespaceString_forTest("b"), nullptr, boost::none);
    PlanEnumeratorContext ctx{graph};
    ctx.enumerateJoinSubsets();

    auto& level0 = ctx.getSubsets(0);
    ASSERT_EQ(NodeSet{"01"}, level0[0].subset);
    ASSERT_EQ(NodeSet{"10"}, level0[1].subset);
    ASSERT_EQ(2, level0.size());

    auto& level1 = ctx.getSubsets(1);
    ASSERT_EQ(NodeSet{"11"}, level1[0].subset);
    ASSERT_EQ(1, level1.size());
}

TEST(JoinPlanEnumerator, InitializeSubsetsThree) {
    JoinGraph graph;
    graph.addNode(NamespaceString::createNamespaceString_forTest("a"), nullptr, boost::none);
    graph.addNode(NamespaceString::createNamespaceString_forTest("b"), nullptr, boost::none);
    graph.addNode(NamespaceString::createNamespaceString_forTest("c"), nullptr, boost::none);
    PlanEnumeratorContext ctx{graph};
    ctx.enumerateJoinSubsets();

    auto& level0 = ctx.getSubsets(0);
    ASSERT_EQ(NodeSet{"001"}, level0[0].subset);
    ASSERT_EQ(NodeSet{"010"}, level0[1].subset);
    ASSERT_EQ(NodeSet{"100"}, level0[2].subset);
    ASSERT_EQ(3, level0.size());

    auto& level1 = ctx.getSubsets(1);
    ASSERT_EQ(NodeSet{"011"}, level1[0].subset);
    ASSERT_EQ(NodeSet{"101"}, level1[1].subset);
    ASSERT_EQ(NodeSet{"110"}, level1[2].subset);
    ASSERT_EQ(3, level1.size());

    auto& level2 = ctx.getSubsets(2);
    ASSERT_EQ(NodeSet{"111"}, level2[0].subset);
    ASSERT_EQ(1, level2.size());
}

// Calculate N choose K. C(n, k) = n! / (k! * (n-k)!).
// Perform this calculation in iterative fashion by alternating multiplication and division avoiding
// explicit factorial calculation to avoid integer overflow problems.
unsigned long long combinations(int n, int k) {
    if (n == k) {
        return 1;
    }
    // Optimization C(n, k) = C(n, n-k)
    k = std::min(k, n - k);
    unsigned long long res = 1;
    for (int i = 1; i <= k; ++i) {
        res = res * (n - i + 1) / i;
    }
    return res;
}

TEST(JoinPlanEnumerator, InitialzeLargeSubsets) {
    // Pick large enough number of nodes to exercise the enumeration code while small enough that
    // the test can finish in a reasonable amount of time.
    constexpr int N = 15;
    JoinGraph graph;
    for (int i = 0; i < N; ++i) {
        graph.addNode(NamespaceString::createNamespaceString_forTest(""), nullptr, boost::none);
    }
    PlanEnumeratorContext ctx{graph};
    ctx.enumerateJoinSubsets();
    ASSERT_EQ(N, ctx.getSubsets(0).size());
    for (int k = 1; k < N; ++k) {
        // The expected number of subsets for the k'th level is N choose k+1 (binomial coefficient).
        int expectedLevelSize = combinations(N, k + 1);
        auto& subsets = ctx.getSubsets(k);
        ASSERT_EQ(expectedLevelSize, subsets.size());
        for (auto&& s : subsets) {
            ASSERT_EQ(k + 1, s.subset.count());
        }
    }
}

}  // namespace mongo::join_ordering
