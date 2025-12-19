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

#include "mongo/db/query/compiler/optimizer/join/join_graph.h"

#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {
TEST(JoinGraphTests, AddNode) {
    MutableJoinGraph graph{};

    auto first = *graph.addNode(makeNSS("first"), nullptr, boost::none);
    auto second = *graph.addNode(makeNSS("second"), nullptr, FieldPath("snd"));

    ASSERT_EQ(graph.numNodes(), 2);
    ASSERT_EQ(graph.numEdges(), 0);

    ASSERT_EQ(graph.getNode(first).collectionName, makeNSS("first"));
    ASSERT_EQ(graph.getNode(second).collectionName, makeNSS("second"));
}

TEST(JoinGraphTests, AddEdge) {
    MutableJoinGraph mgraph{};

    auto first = *mgraph.addNode(makeNSS("first"), nullptr, boost::none);
    auto second = *mgraph.addNode(makeNSS("second"), nullptr, FieldPath("snd"));
    auto third = *mgraph.addNode(makeNSS("third"), nullptr, FieldPath("trd"));


    auto firstSecond = mgraph.addSimpleEqualityEdge(first, second, 0, 1);
    auto secondThird = mgraph.addSimpleEqualityEdge(second, third, 2, 3);

    JoinGraph graph(std::move(mgraph));

    ASSERT_EQ(graph.numNodes(), 3);
    ASSERT_EQ(graph.numEdges(), 2);

    ASSERT_TRUE(firstSecond.has_value());
    ASSERT_TRUE(secondThird.has_value());

    ASSERT_EQ(graph.getEdge(*firstSecond).left, first);
    ASSERT_EQ(graph.getEdge(*firstSecond).right, second);
    ASSERT_EQ(graph.getEdge(*secondThird).left, second);
    ASSERT_EQ(graph.getEdge(*secondThird).right, third);

    ASSERT_EQ(graph.findEdge(first, second), firstSecond);
    ASSERT_EQ(graph.findEdge(second, first), firstSecond);
    ASSERT_EQ(graph.findEdge(second, third), secondThird);
    ASSERT_EQ(graph.findEdge(third, second), secondThird);
    ASSERT_EQ(graph.findEdge(first, third), boost::none);
    ASSERT_EQ(graph.findEdge(third, first), boost::none);
}

DEATH_TEST(JoinGraphTestsDeathTest,
           AddEdgeSimpleSelfEdgeForbidden,
           "Self edges are not permitted") {
    MutableJoinGraph graph{};

    auto a = *graph.addNode(makeNSS("a"), nullptr, boost::none);
    *graph.addNode(makeNSS("b"), nullptr, boost::none);

    // Try adding a self-edge (a) -- (a).
    graph.addSimpleEqualityEdge(a, a, 0, 1);
}

TEST(JoinGraphTests, getJoinEdges) {
    MutableJoinGraph mgraph{};

    auto a = *mgraph.addNode(makeNSS("a"), nullptr, boost::none);
    auto b = *mgraph.addNode(makeNSS("b"), nullptr, FieldPath("b"));
    auto c = *mgraph.addNode(makeNSS("c"), nullptr, FieldPath("c"));
    auto d = *mgraph.addNode(makeNSS("d"), nullptr, FieldPath("d"));

    auto ab = *mgraph.addSimpleEqualityEdge(a, b, 0, 1);
    auto ac = *mgraph.addSimpleEqualityEdge(a, c, 2, 3);
    auto cd = *mgraph.addSimpleEqualityEdge(c, d, 4, 5);

    JoinGraph graph(std::move(mgraph));

    ASSERT_EQ(graph.getJoinEdges(NodeSet{"0001"}, NodeSet{"0010"}), std::vector<EdgeId>{ab});
    ASSERT_EQ(graph.getJoinEdges(NodeSet{"0001"}, NodeSet{"0100"}), std::vector<EdgeId>{ac});
    ASSERT_EQ(graph.getJoinEdges(NodeSet{"0100"}, NodeSet{"1000"}), std::vector<EdgeId>{cd});
    ASSERT_EQ(graph.getJoinEdges(NodeSet{"0001"}, NodeSet{"0110"}), (std::vector<EdgeId>{ab, ac}));
    ASSERT_EQ(graph.getJoinEdges(NodeSet{"1001"}, NodeSet{"0110"}),
              (std::vector<EdgeId>{ab, ac, cd}));
    ASSERT_EQ(graph.getJoinEdges(NodeSet{"0001"}, NodeSet{"1000"}), std::vector<EdgeId>{});
    ASSERT_EQ(graph.getJoinEdges(NodeSet{"1001"}, NodeSet{"1000"}), std::vector<EdgeId>{});
}

TEST(JoinGraph, MultiplePredicatesSameEdge) {
    MutableJoinGraph mgraph{};

    auto a = *mgraph.addNode(makeNSS("a"), nullptr, boost::none);
    auto b = *mgraph.addNode(makeNSS("b"), nullptr, boost::none);

    auto ab = *mgraph.addSimpleEqualityEdge(a, b, 0, 1);
    auto ab2 = *mgraph.addSimpleEqualityEdge(a, b, 2, 3);
    // Opposite order of nodes
    auto ab3 = *mgraph.addSimpleEqualityEdge(b, a, 5, 4);

    // Edge is deduplicated
    ASSERT_EQ(ab, ab2);
    ASSERT_EQ(ab2, ab3);

    JoinGraph graph(std::move(mgraph));
    auto edges = graph.getJoinEdges(makeNodeSet(a), makeNodeSet(b));

    ASSERT_EQ(1, edges.size());
    // Edge returned when order of nodes reversed
    ASSERT_EQ(edges, graph.getJoinEdges(makeNodeSet(b), makeNodeSet(a)));

    auto edge = graph.getEdge(edges[0]);

    ASSERT_EQ(edge.left, a);
    ASSERT_EQ(edge.right, b);
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "predicates": [
                {
                    "op": "eq",
                    "left": 0,
                    "right": 1
                },
                {
                    "op": "eq",
                    "left": 2,
                    "right": 3
                },
                {
                    "op": "eq",
                    "left": 4,
                    "right": 5
                }
            ],
            "left": {"$numberInt":"0"},
            "right": {"$numberInt":"1"}
        })",
        edge.toBSON());
}

TEST(JoinGraphTests, GetNeighborsSimpleEqualityEdges_NoEdges) {
    MutableJoinGraph mgraph{};

    auto a = *mgraph.addNode(makeNSS("a"), nullptr, boost::none);
    auto b = *mgraph.addNode(makeNSS("b"), nullptr, boost::none);
    auto c = *mgraph.addNode(makeNSS("c"), nullptr, boost::none);
    auto d = *mgraph.addNode(makeNSS("d"), nullptr, boost::none);
    auto e = *mgraph.addNode(makeNSS("e"), nullptr, boost::none);

    JoinGraph graph(std::move(mgraph));

    // At this point, no edges.
    ASSERT_EQ(graph.getNeighbors(a), NodeSet{});
    ASSERT_EQ(graph.getNeighbors(b), NodeSet{});
    ASSERT_EQ(graph.getNeighbors(c), NodeSet{});
    ASSERT_EQ(graph.getNeighbors(d), NodeSet{});
    ASSERT_EQ(graph.getNeighbors(e), NodeSet{});
}

TEST(JoinGraphTests, GetNeighborsSimpleEqualityEdges_WithEdges) {
    MutableJoinGraph mgraph{};

    auto a = *mgraph.addNode(makeNSS("a"), nullptr, boost::none);
    auto b = *mgraph.addNode(makeNSS("b"), nullptr, boost::none);
    auto c = *mgraph.addNode(makeNSS("c"), nullptr, boost::none);
    auto d = *mgraph.addNode(makeNSS("d"), nullptr, boost::none);
    auto e = *mgraph.addNode(makeNSS("e"), nullptr, boost::none);

    // Now add edges: a -- b, a -- c, c -- d.
    mgraph.addSimpleEqualityEdge(a, b, 0, 1);
    mgraph.addSimpleEqualityEdge(a, c, 2, 3);
    mgraph.addSimpleEqualityEdge(c, d, 4, 5);

    JoinGraph graph(std::move(mgraph));

    // A connected to b,c. B connected to a. C connected to a,d. D connected to c. E not
    // connected.
    ASSERT_EQ(graph.getNeighbors(a), makeNodeSet(b, c));
    ASSERT_EQ(graph.getNeighbors(b), makeNodeSet(a));
    ASSERT_EQ(graph.getNeighbors(c), makeNodeSet(a, d));
    ASSERT_EQ(graph.getNeighbors(d), makeNodeSet(c));
    ASSERT_EQ(graph.getNeighbors(e), NodeSet{});
}

TEST(JoinGraph, GetNeighborsMultiEdges) {
    MutableJoinGraph mgraph{};

    auto a = *mgraph.addNode(makeNSS("a"), nullptr, boost::none);
    auto b = *mgraph.addNode(makeNSS("b"), nullptr, boost::none);
    auto c = *mgraph.addNode(makeNSS("c"), nullptr, boost::none);

    // Now add two edges between "a" and "b". These could in theory be simplified into a single
    // complex edge with multiple predicates, but this demonstrates that getNeighbors supports it.
    mgraph.addEdge(a, b, {});
    mgraph.addEdge(b, a, {});

    JoinGraph graph(std::move(mgraph));

    // A connected to b. B connected to a.
    ASSERT_EQ(graph.getNeighbors(a), makeNodeSet(b));
    ASSERT_EQ(graph.getNeighbors(b), makeNodeSet(a));
    ASSERT_EQ(graph.getNeighbors(c), NodeSet{});
}

TEST(JoinGraph, GetNeighborsCycle) {
    MutableJoinGraph mgraph{};

    auto a = *mgraph.addNode(makeNSS("a"), nullptr, boost::none);
    auto b = *mgraph.addNode(makeNSS("b"), nullptr, boost::none);
    auto c = *mgraph.addNode(makeNSS("c"), nullptr, boost::none);
    auto d = *mgraph.addNode(makeNSS("d"), nullptr, boost::none);

    // Introduce a cycle involving all four nodes.
    mgraph.addEdge(a, b, {});
    mgraph.addEdge(b, c, {});
    mgraph.addEdge(c, d, {});
    mgraph.addEdge(d, a, {});

    JoinGraph graph(std::move(mgraph));

    // Each node is connected to only two others.
    ASSERT_EQ(graph.getNeighbors(a), makeNodeSet(b, d));
    ASSERT_EQ(graph.getNeighbors(b), makeNodeSet(a, c));
    ASSERT_EQ(graph.getNeighbors(c), makeNodeSet(b, d));
    ASSERT_EQ(graph.getNeighbors(d), makeNodeSet(a, c));
}

namespace {
void assertEdgesEq(std::vector<EdgeId> a, std::vector<EdgeId> b) {
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    ASSERT_EQ(a, b);
}
}  // namespace

TEST(JoinGraph, GetEdgesForSubgraph) {
    /** Construct a graph like so
     * a -- b -- c
     *          / \
     *         d -- e
     */
    MutableJoinGraph mgraph{};
    auto a = *mgraph.addNode(makeNSS("a"), nullptr, boost::none);
    auto b = *mgraph.addNode(makeNSS("b"), nullptr, FieldPath("b"));
    auto c = *mgraph.addNode(makeNSS("c"), nullptr, FieldPath("c"));
    auto d = *mgraph.addNode(makeNSS("d"), nullptr, FieldPath("d"));
    auto e = *mgraph.addNode(makeNSS("e"), nullptr, FieldPath("d"));

    auto ab = *mgraph.addSimpleEqualityEdge(a, b, 0, 1);
    auto bc = *mgraph.addSimpleEqualityEdge(b, c, 1, 2);
    auto cd = *mgraph.addSimpleEqualityEdge(c, d, 2, 3);
    auto de = *mgraph.addSimpleEqualityEdge(d, e, 3, 4);
    auto ce = *mgraph.addSimpleEqualityEdge(c, e, 2, 4);

    JoinGraph graph(std::move(mgraph));

    assertEdgesEq(graph.getEdgesForSubgraph(makeNodeSet(a)), std::vector<EdgeId>{});

    assertEdgesEq(graph.getEdgesForSubgraph(makeNodeSet(a, b)), std::vector<EdgeId>{ab});
    assertEdgesEq(graph.getEdgesForSubgraph(makeNodeSet(a, c)), std::vector<EdgeId>{});
    assertEdgesEq(graph.getEdgesForSubgraph(makeNodeSet(c, d)), std::vector<EdgeId>{cd});

    assertEdgesEq(graph.getEdgesForSubgraph(makeNodeSet(a, b, c)), (std::vector<EdgeId>{ab, bc}));
    assertEdgesEq(graph.getEdgesForSubgraph(makeNodeSet(a, b, e)), std::vector<EdgeId>{ab});

    assertEdgesEq(graph.getEdgesForSubgraph(makeNodeSet(a, b, c, d)),
                  (std::vector<EdgeId>{ab, bc, cd}));
    assertEdgesEq(graph.getEdgesForSubgraph(makeNodeSet(a, b, c, e)),
                  (std::vector<EdgeId>{ab, bc, ce}));
    assertEdgesEq(graph.getEdgesForSubgraph(makeNodeSet(a, b, d, e)),
                  (std::vector<EdgeId>{ab, de}));

    assertEdgesEq(graph.getEdgesForSubgraph(makeNodeSet(a, b, c, d, e)),
                  (std::vector<EdgeId>{ab, bc, cd, de, ce}));
}

TEST(JoinGraph, BuildParams) {
    JoinGraphBuildParams buildParams(16, 32, 64);
    MutableJoinGraph mgraph(buildParams);

    // Case 1: Validate the maximum number of nodes.
    std::vector<NodeId> nodeIds;
    nodeIds.reserve(buildParams.maxNodesInJoin);

    for (size_t i = 0; i < buildParams.maxNodesInJoin + 10; ++i) {
        auto nodeId = mgraph.addNode(makeNSS("a"), nullptr, boost::none);
        if (nodeId.has_value()) {
            nodeIds.emplace_back(*nodeId);
        }
    }
    // Validate that the number of nodes is of the expected size.
    ASSERT_EQ(mgraph.numNodes(), buildParams.maxNodesInJoin);
    // Validate that the nodes creation status was correctly reported.
    ASSERT_EQ(nodeIds.size(), buildParams.maxNodesInJoin);

    PathId pathId{100};

    // Case 2. Validate the maximum number of edges.
    std::vector<EdgeId> edgeIds;
    edgeIds.reserve(buildParams.maxEdgesInJoin);
    for (auto left : nodeIds) {
        for (auto right : nodeIds) {
            if (left >= right) {
                continue;
            }
            auto edgeId = mgraph.addSimpleEqualityEdge(left, right, pathId + 1, pathId + 2);
            if (edgeId.has_value()) {
                edgeIds.emplace_back(*edgeId);
                pathId += 2;
            }
        }
    }
    // Validate that the number of edges is of the expected size.
    ASSERT_EQ(mgraph.numEdges(), buildParams.maxEdgesInJoin);
    // Validate that the edges creation status was correctly reported.
    ASSERT_EQ(edgeIds.size(), buildParams.maxEdgesInJoin);

    // Case 3. Validate the maximum number of predicates.
    auto edge = mgraph.edges().back();
    size_t numPredicates = mgraph.numEdges();
    for (size_t i = 0; i < buildParams.maxPredicatesInJoin + 10; ++i) {
        auto edgeId = mgraph.addSimpleEqualityEdge(edge.right, edge.left, pathId + 1, pathId + 2);
        if (edgeId.has_value()) {
            ++numPredicates;
            pathId += 2;
        }
    }
    // Validate that the predicates creation status was correctly reported.
    ASSERT_EQ(numPredicates, buildParams.maxPredicatesInJoin);
}
}  // namespace mongo::join_ordering
