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
    JoinGraph graph{};

    auto first = *graph.addNode(makeNSS("first"), nullptr, boost::none);
    auto second = *graph.addNode(makeNSS("second"), nullptr, FieldPath("snd"));

    ASSERT_EQ(graph.numNodes(), 2);
    ASSERT_EQ(graph.numEdges(), 0);

    ASSERT_EQ(graph.getNode(first).collectionName, makeNSS("first"));
    ASSERT_EQ(graph.getNode(second).collectionName, makeNSS("second"));
}

TEST(JoinGraphTests, AddEdge) {
    JoinGraph graph{};

    auto first = *graph.addNode(makeNSS("first"), nullptr, boost::none);
    auto second = *graph.addNode(makeNSS("second"), nullptr, FieldPath("snd"));
    auto third = *graph.addNode(makeNSS("third"), nullptr, FieldPath("trd"));


    auto firstSecond = graph.addSimpleEqualityEdge(first, second, 0, 1);
    auto secondThird = graph.addSimpleEqualityEdge(second, third, 2, 3);


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
    JoinGraph graph{};

    auto a = *graph.addNode(makeNSS("a"), nullptr, boost::none);
    *graph.addNode(makeNSS("b"), nullptr, boost::none);

    // Try adding a self-edge (a) -- (a).
    graph.addSimpleEqualityEdge(a, a, 0, 1);
}

TEST(JoinGraphTests, getJoinEdges) {
    JoinGraph graph{};

    auto a = *graph.addNode(makeNSS("a"), nullptr, boost::none);
    auto b = *graph.addNode(makeNSS("b"), nullptr, FieldPath("b"));
    auto c = *graph.addNode(makeNSS("c"), nullptr, FieldPath("c"));
    auto d = *graph.addNode(makeNSS("d"), nullptr, FieldPath("d"));

    auto ab = *graph.addSimpleEqualityEdge(a, b, 0, 1);
    auto ac = *graph.addSimpleEqualityEdge(a, c, 2, 3);
    auto cd = *graph.addSimpleEqualityEdge(c, d, 4, 5);

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
    JoinGraph graph{};

    auto a = *graph.addNode(makeNSS("a"), nullptr, boost::none);
    auto b = *graph.addNode(makeNSS("b"), nullptr, boost::none);

    auto ab = *graph.addSimpleEqualityEdge(a, b, 0, 1);
    auto ab2 = *graph.addSimpleEqualityEdge(a, b, 2, 3);
    // Opposite order of nodes
    auto ab3 = *graph.addSimpleEqualityEdge(b, a, 5, 4);

    // Edge is deduplicated
    ASSERT_EQ(ab, ab2);
    ASSERT_EQ(ab2, ab3);

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

TEST(JoinGraphTests, GetNeighborsSimpleEqualityEdges) {
    JoinGraph graph{};

    auto a = *graph.addNode(makeNSS("a"), nullptr, boost::none);
    auto b = *graph.addNode(makeNSS("b"), nullptr, boost::none);
    auto c = *graph.addNode(makeNSS("c"), nullptr, boost::none);
    auto d = *graph.addNode(makeNSS("d"), nullptr, boost::none);
    auto e = *graph.addNode(makeNSS("e"), nullptr, boost::none);

    // At this point, no edges.
    ASSERT_EQ(graph.getNeighbors(a), NodeSet{});
    ASSERT_EQ(graph.getNeighbors(b), NodeSet{});
    ASSERT_EQ(graph.getNeighbors(c), NodeSet{});
    ASSERT_EQ(graph.getNeighbors(d), NodeSet{});
    ASSERT_EQ(graph.getNeighbors(e), NodeSet{});

    // Now add edges: a -- b, a -- c, c -- d.
    graph.addSimpleEqualityEdge(a, b, 0, 1);
    graph.addSimpleEqualityEdge(a, c, 2, 3);
    graph.addSimpleEqualityEdge(c, d, 4, 5);

    // A connected to b,c. B connected to a. C connected to a,d. D connected to c. E not connected.
    ASSERT_EQ(graph.getNeighbors(a), makeNodeSet(b, c));
    ASSERT_EQ(graph.getNeighbors(b), makeNodeSet(a));
    ASSERT_EQ(graph.getNeighbors(c), makeNodeSet(a, d));
    ASSERT_EQ(graph.getNeighbors(d), makeNodeSet(c));
    ASSERT_EQ(graph.getNeighbors(e), NodeSet{});
}

TEST(JoinGraph, GetNeighborsMultiEdges) {
    JoinGraph graph{};

    auto a = *graph.addNode(makeNSS("a"), nullptr, boost::none);
    auto b = *graph.addNode(makeNSS("b"), nullptr, boost::none);
    auto c = *graph.addNode(makeNSS("c"), nullptr, boost::none);

    // Now add two edges between "a" and "b". These could in theory be simplified into a single
    // complex edge with multiple predicates, but this demonstrates that getNeighbors supports it.
    graph.addEdge(a, b, {});
    graph.addEdge(b, a, {});

    // A connected to b. B connected to a.
    ASSERT_EQ(graph.getNeighbors(a), makeNodeSet(b));
    ASSERT_EQ(graph.getNeighbors(b), makeNodeSet(a));
    ASSERT_EQ(graph.getNeighbors(c), NodeSet{});
}

TEST(JoinGraph, GetNeighborsCycle) {
    JoinGraph graph{};

    auto a = *graph.addNode(makeNSS("a"), nullptr, boost::none);
    auto b = *graph.addNode(makeNSS("b"), nullptr, boost::none);
    auto c = *graph.addNode(makeNSS("c"), nullptr, boost::none);
    auto d = *graph.addNode(makeNSS("d"), nullptr, boost::none);

    // Introduce a cycle involving all four nodes.
    graph.addEdge(a, b, {});
    graph.addEdge(b, c, {});
    graph.addEdge(c, d, {});
    graph.addEdge(d, a, {});

    // Each node is connected to only two others.
    ASSERT_EQ(graph.getNeighbors(a), makeNodeSet(b, d));
    ASSERT_EQ(graph.getNeighbors(b), makeNodeSet(a, c));
    ASSERT_EQ(graph.getNeighbors(c), makeNodeSet(b, d));
    ASSERT_EQ(graph.getNeighbors(d), makeNodeSet(a, c));
}
}  // namespace mongo::join_ordering
