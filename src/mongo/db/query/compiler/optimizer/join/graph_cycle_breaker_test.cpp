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
#include "mongo/util/assert_util.h"

namespace mongo::join_ordering {
namespace {
/**
 * Return the number of cycles in a clique of 'numNodes'.
 */
constexpr size_t cyclesInClique(size_t numNodes) {
    switch (numNodes) {
        case 0:
        case 1:
        case 2:
            return 0;
        case 3:
            return 1;
        case 4:
            return 7;
        case 5:
            return 37;
        case 6:
            return 197;
        case 7:
            return 1172;
        case 8:
            return 8018;
        case 9:
            return 62814;
        default:
            MONGO_UNREACHABLE_TASSERT(11509310);
    }
}

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

// ***************************************************
// findCycles tests

class FindCyclesTest : public unittest::Test {
public:
    /**
     * Append a clique of size 'end' - 'begin' nodes, starting with node 'begin' to 'edges'.
     */
    static void appendClique(PathId begin,
                             PathId end,
                             std::vector<std::pair<PathId, PathId>>& edges) {
        for (PathId left = begin; left != end; ++left) {
            for (PathId right = left + 1; right != end; ++right) {
                edges.emplace_back(left, right);
            }
        }
    }

    static AdjacencyList makeAdjacencyList(const std::vector<std::pair<PathId, PathId>>& edges) {
        const auto maxPathId = [&edges]() {
            PathId maxPathId = 0;
            for (auto [left, right] : edges) {
                maxPathId = std::max({maxPathId, left, right});
            }
            return maxPathId;
        }();

        AdjacencyList adjList;
        adjList.neighbors.resize(maxPathId + 1);

        EdgeId edgeId = 0;
        for (auto [left, right] : edges) {
            const PredicateId predicateId = static_cast<PredicateId>(adjList.predicates.size());
            adjList.neighbors[left].emplace_back(right, predicateId);
            adjList.neighbors[right].emplace_back(left, predicateId);
            adjList.predicates.emplace_back(edgeId++, 0);
        }
        return adjList;
    }

    void testFindCycles(AdjacencyList adjList, std::vector<Bitset> expected) {
        auto actual = findCycles(std::move(adjList));
        std::sort(actual.cycles.begin(), actual.cycles.end());
        std::sort(expected.begin(), expected.end());
        ASSERT_EQ(actual.cycles, expected);
    }

    void testFindCycles(AdjacencyList adjList, size_t expectedNumberOfCycles) {
        auto actual = findCycles(std::move(adjList));
        ASSERT_EQ(actual.cycles.size(), expectedNumberOfCycles);
    }
};

TEST_F(FindCyclesTest, GraphWithComplexCycles) {
    // Edges: A - B, B - C, C - D, D - A, C - A
    auto adjList = makeAdjacencyList({{0, 1}, {1, 2}, {2, 3}, {3, 0}, {2, 0}});

    // Three cycles:
    // * 01111: A - B - C - D - A
    // * 10011: A - B - C - A
    // * 11100: A - C - D - A
    std::vector<Bitset> expected{Bitset{"01111"}, Bitset{"10011"}, Bitset{"11100"}};

    testFindCycles(std::move(adjList), std::move(expected));
}

TEST_F(FindCyclesTest, NoCycles) {
    // A - B, B - C
    auto adjList = makeAdjacencyList({{0, 1}, {1, 2}});
    testFindCycles(std::move(adjList), {});
}

TEST_F(FindCyclesTest, DisconnectedGraph) {
    // A - B, C - D
    auto adjList = makeAdjacencyList({{0, 1}, {2, 3}});
    testFindCycles(std::move(adjList), {});
}

TEST_F(FindCyclesTest, DisconnectedGraphWithCycles) {
    auto adjList = makeAdjacencyList({
        // A - B - C
        {0, 1},
        {1, 2},
        {2, 0},
        // D - E - F - D
        {3, 4},
        {4, 5},
        {5, 6},
        {6, 3},
    });

    std::vector<Bitset> expected{Bitset{"0000111"}, Bitset{"1111000"}};

    testFindCycles(std::move(adjList), std::move(expected));
}

/**
 * A clique and some cycle that includes members of the clique.
 */
TEST_F(FindCyclesTest, CliqueOf4AndCycle) {
    enum Nodes { a, b, c, d, e, f };
    // Edges: 0: AB, 1: BC, 2: CD, 3: AD, 4: AC, 5: BD, 6: ED, 7: EF, 8: CF
    // A, B, C, D form a clique.
    auto adjList =
        makeAdjacencyList({{a, b}, {b, c}, {c, d}, {a, d}, {a, c}, {d, b}, {e, d}, {e, f}, {c, f}});

    // 7 cycles in the clique:
    std::vector<Bitset> expected{
        Bitset{"000001111"},  // A - B - C - D - A
        Bitset{"000010011"},  // A - B - C - A
        Bitset{"000011100"},  // A - C - D - A
        Bitset{"000100110"},  // B - C - D - B
        Bitset{"000101001"},  // A - B - D - A
        Bitset{"000110101"},  // A - B - D - C - A
        Bitset{"000111010"},  // A - D - B - C - A
        Bitset{"111110001"},  // A - B - D - E - F - C - A
        Bitset{"111001011"},  // A - D - E - F - C - B - A
        Bitset{"111011000"},  // A - D - E - F - C - A
        Bitset{"111100010"},  // B - D - E - F - C - B
        Bitset{"111000100"},  // C - D - E - F - C
    };

    testFindCycles(std::move(adjList), std::move(expected));
}


TEST_F(FindCyclesTest, Cliques) {
    for (PathId size = 3; size < 8; ++size) {
        std::vector<std::pair<PathId, PathId>> edges;
        appendClique(0, size, edges);
        auto adjList = makeAdjacencyList(std::move(edges));
        testFindCycles(std::move(adjList), cyclesInClique(size));
    }
}

/**
 * Multiple clique in a graph plus chains of nodes which do not participate in cycles, yet makes the
 * cycle search harder.
 */
TEST_F(FindCyclesTest, MultipleCliques) {
    PathId nextPathId = 0;
    size_t expectedNumberOfCycles = 0;
    std::vector<std::pair<PathId, PathId>> edges;

    auto addClique = [&](auto size) {
        appendClique(nextPathId, nextPathId + size, edges);
        nextPathId += size;
        expectedNumberOfCycles += cyclesInClique(size);
    };

    auto addChain = [&](auto size) {
        PathId end = nextPathId + size;
        for (; nextPathId != end; ++nextPathId) {
            edges.emplace_back(nextPathId, nextPathId + 1);
        }
    };

    addChain(5);
    addClique(3);
    nextPathId += 2;  // skip some nodes
    addClique(5);
    addChain(3);
    addClique(3);
    addClique(4);
    addChain(7);

    auto adjList = makeAdjacencyList(std::move(edges));
    testFindCycles(std::move(adjList), expectedNumberOfCycles);
}
}  // namespace
}  // namespace mongo::join_ordering
