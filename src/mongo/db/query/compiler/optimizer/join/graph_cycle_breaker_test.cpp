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


#include "mongo/db/query/compiler/optimizer/join/graph_cycle_breaker.h"

#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/unittest/unittest.h"


namespace mongo::join_ordering {
class GraphCycleBreakerTest : public unittest::Test {
public:
    GraphCycleBreakerTest() : graph{}, breaker(graph) {
        a = addNode("a");
        b = addNode("b");
        c = addNode("c");
        d = addNode("d");
        e = addNode("e");
        f = addNode("f");
    }

    NodeId addNode(StringData collName) {
        return *graph.addNode(makeNSS(collName), nullptr, {});
    }

    EdgeId addEdge(NodeId u, NodeId v) {
        return *graph.addEdge(u, v, {});
    }

    template <typename... EdgeIds>
    std::vector<EdgeId> breakCycles(EdgeIds... edges) {
        return breaker.breakCycles({edges...});
    }

    JoinGraph graph;
    GraphCycleBreaker breaker;
    NodeId a, b, c, d, e, f;
};

TEST_F(GraphCycleBreakerTest, GraphWithCycle) {
    auto edge_ab = addEdge(a, b);
    auto edge_bc = addEdge(b, c);
    auto edge_cd = addEdge(c, d);
    auto edge_da = addEdge(d, a);
    auto edge_ca = addEdge(c, a);

    auto newEdges = breakCycles(edge_ab, edge_bc, edge_cd, edge_da, edge_ca);
    //  Two edges are expected to be removed.
    ASSERT_EQ(newEdges.size(), 3);

    newEdges = breakCycles(edge_ab, edge_bc, edge_ca);
    // Subgraph case.
    ASSERT_EQ(newEdges.size(), 2);
}

TEST_F(GraphCycleBreakerTest, NoCycleGraph) {
    auto edge_ab = addEdge(a, b);
    auto edge_bc = addEdge(b, c);
    auto edge_cd = addEdge(c, d);

    auto newEdges = breakCycles(edge_ab, edge_bc, edge_cd);
    // No edges are expected to be removed.
    ASSERT_EQ(newEdges.size(), 3);
}

TEST_F(GraphCycleBreakerTest, DisconnectedGraph) {
    auto edge_ab = addEdge(a, b);
    auto edge_cd = addEdge(c, d);

    auto newEdges = breakCycles(edge_ab, edge_cd);
    // No edges are expected to be removed.
    ASSERT_EQ(newEdges.size(), 2);
}

TEST_F(GraphCycleBreakerTest, DisconnectedGraphWithCycles) {
    // A - B - C  - A
    auto edge_ab = addEdge(a, b);
    auto edge_bc = addEdge(b, c);
    auto edge_ca = addEdge(c, a);

    // D - E - F - D
    auto edge_de = addEdge(d, e);
    auto edge_ef = addEdge(e, f);
    auto edge_fd = addEdge(f, d);

    auto newEdges = breakCycles(edge_ab, edge_bc, edge_ca, edge_de, edge_ef, edge_fd);
    // Two edges are expected to be removed.
    ASSERT_EQ(newEdges.size(), 4);
}
}  // namespace mongo::join_ordering
