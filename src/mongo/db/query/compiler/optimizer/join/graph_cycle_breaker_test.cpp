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
    static constexpr EdgeId kMaxEdgeId = 63;

    using PathMap = absl::flat_hash_map<NodeId, PathId>;
    GraphCycleBreakerTest() : graph{} {
        a = addNode("a");
        b = addNode("b");
        c = addNode("c");
        d = addNode("d");
        e = addNode("e");
        f = addNode("f");

        defaultPaths = {{a, pa}, {b, pb}, {c, pc}, {d, pd}, {e, pe}, {f, pf}};
        alternativePaths = {{a, pa1}, {b, pb1}, {c, pc1}, {d, pd1}, {e, pe1}, {f, pf1}};

        for (size_t i = 0; i <= kMaxEdgeId; ++i) {
            edgeSelectivities.emplace_back(
                cost_based_ranker::SelectivityType{static_cast<double>(i + 1) / (2.0 * kMaxEdgeId)},
                cost_based_ranker::EstimationSource::Code);
        }
    }

    NodeId addNode(StringData collName) {
        return *graph.addNode(makeNSS(collName), nullptr, {});
    }

    EdgeId addEdge(NodeId u, NodeId v) {
        return addEdge(u, v, defaultPaths);
    }

    EdgeId addEdge(NodeId u, NodeId v, PathMap pathMap) {
        return *graph.addSimpleEqualityEdge(u, v, pathMap[u], pathMap[v]);
    }

    EdgeId addEdge(NodeId u, NodeId v, PathId uPath, PathId vPath) {
        return *graph.addSimpleEqualityEdge(u, v, uPath, vPath);
    }

    EdgeId addCompoundEdge(NodeId u, NodeId v) {
        addEdge(u, v, defaultPaths);
        return addEdge(u, v, alternativePaths);
    }

    void validateCycles(size_t expectedNumberOfCycles) {
        ASSERT_LTE(graph.numEdges(), kMaxEdgeId + 1);

        auto breaker = makeCycleBreaker();
        ASSERT_EQ(breaker.numCycles_forTest(), expectedNumberOfCycles);
    }

    template <typename... EdgeIds>
    std::vector<EdgeId> breakCycles(EdgeIds... edges) {
        ASSERT_LTE(graph.numEdges(), kMaxEdgeId + 1);

        auto breaker = makeCycleBreaker();
        auto newEdges = breaker.breakCycles({edges...});
        return newEdges;
    }

    void setSelectivity(EdgeId edgeId, double newSelectivity) {
        edgeSelectivities[edgeId] = cost_based_ranker::SelectivityEstimate(
            cost_based_ranker::SelectivityType(newSelectivity),
            cost_based_ranker::EstimationSource::Code);
    }

    GraphCycleBreaker makeCycleBreaker() {
        if (!joinGraphSrorage.has_value()) {
            joinGraphSrorage = JoinGraph(std::move(graph));
        }
        return GraphCycleBreaker{joinGraphSrorage.get(), edgeSelectivities};
    }

    static constexpr PathId p1{1}, p2{2}, pa{11}, pb{12}, pc{13}, pd{14}, pe{15}, pf{16}, pa1{21},
        pb1{22}, pc1{23}, pd1{24}, pe1{25}, pf1{26}, pEnd{27};

    MutableJoinGraph graph;
    boost::optional<JoinGraph> joinGraphSrorage;
    NodeId a, b, c, d, e, f;
    absl::flat_hash_map<NodeId, PathId> defaultPaths;
    absl::flat_hash_map<NodeId, PathId> alternativePaths;
    EdgeSelectivities edgeSelectivities;
};

/**
 * Tests that a middle by selectivity edge is always selected to break cycles.
 */
TEST_F(GraphCycleBreakerTest, MiddleEdge) {
    auto edge_ab = addCompoundEdge(a, b);
    auto edge_bc = addEdge(b, c);
    auto edge_ca = addEdge(c, a);

    // B-C is a middle by selectivity.
    setSelectivity(edge_ab, 0.003);
    setSelectivity(edge_bc, 0.005);
    setSelectivity(edge_ca, 0.007);

    // Case1: B-C is the middle. Selectivities: 0.003, 0.005, 0.007.
    std::vector<EdgeId> expectedEdges{edge_ab, edge_ca};

    auto newEdges = breakCycles(edge_ab, edge_bc, edge_ca);
    std::sort(newEdges.begin(), newEdges.end());
    ASSERT_EQ(newEdges, expectedEdges);

    // Case 2: C-A is the middle. Selectivities: 0.003, 0.009, 0.007.
    // Increase B-C selectivity which makes C-A a new middle.
    setSelectivity(edge_bc, 0.009);
    expectedEdges = {edge_ab, edge_bc};

    newEdges = breakCycles(edge_ab, edge_bc, edge_ca);
    std::sort(newEdges.begin(), newEdges.end());
    ASSERT_EQ(newEdges, expectedEdges);

    // Case 3: Compound edge A-B removal. Selectivities: 0.008, 0.009, 0.007.
    // Increase A-B selectivity which makes A-B a new middle.
    setSelectivity(edge_ab, 0.008);
    expectedEdges = {edge_bc, edge_ca};

    newEdges = breakCycles(edge_ab, edge_bc, edge_ca);
    std::sort(newEdges.begin(), newEdges.end());
    ASSERT_EQ(newEdges, expectedEdges);
}

/**
 * Tests that a non-compund edge is selected if available to break a cycle of size > 3.
 */
TEST_F(GraphCycleBreakerTest, NoCompoundEdgeSelection) {
    auto edge_ab = addCompoundEdge(a, b);
    auto edge_bc = addCompoundEdge(b, c);
    auto edge_cd = addEdge(c, d);
    auto edge_da = addCompoundEdge(d, a);

    // We expected that the only non-compound edge C-D will be removed to break the 4-cycle.
    std::vector<EdgeId> expectedEdges{edge_ab, edge_bc, edge_da};

    auto newEdges = breakCycles(edge_ab, edge_bc, edge_cd, edge_da);
    std::sort(newEdges.begin(), newEdges.end());
    ASSERT_EQ(newEdges, expectedEdges);
}

/**
 * The cycles still breaks even if all edges are compound.
 */
TEST_F(GraphCycleBreakerTest, AllEdgesAreCompound) {
    auto edge_ab = addCompoundEdge(a, b);
    auto edge_bc = addCompoundEdge(b, c);
    auto edge_cd = addCompoundEdge(c, d);
    auto edge_da = addCompoundEdge(d, a);

    auto newEdges = breakCycles(edge_ab, edge_bc, edge_cd, edge_da);

    // One edge was removed to break the cycle.
    ASSERT_EQ(newEdges.size(), 3);
}

TEST_F(GraphCycleBreakerTest, GraphWithComplexCycles) {
    // A.a = B.a and B.a = C.a and C.a = D.a and D.a = A.a and C.a = A.a
    // Three cycles:
    // * A.a - B.a - C.a - D.a - A.a
    // * A.a - B.a - C.a - A.a
    // * A.a - C.a - D.a - A.a
    auto edge_ab = addEdge(a, b);
    auto edge_bc = addEdge(b, c);
    auto edge_cd = addEdge(c, d);
    auto edge_da = addEdge(d, a);
    auto edge_ca = addEdge(c, a);
    // This edge is ignored due to its incompatible predicates.
    auto edge_db = addEdge(d, b, alternativePaths);

    validateCycles(/*expectedNumberOfCycles*/ 3);

    auto newEdges = breakCycles(edge_ab, edge_bc, edge_cd, edge_da, edge_ca, edge_db);
    //  Two edges are expected to be removed.
    ASSERT_EQ(newEdges.size(), 4);

    newEdges = breakCycles(edge_ab, edge_bc, edge_ca);
    // Subgraph case.
    ASSERT_EQ(newEdges.size(), 2);
}

TEST_F(GraphCycleBreakerTest, NoCycleGraph) {
    // A.a = B.a and B.a = C.a
    auto edge_ab = addEdge(a, b);
    auto edge_bc = addEdge(b, c);
    auto edge_cd = addEdge(c, d);

    validateCycles(/*expectedNumberOfCycles*/ 0);

    auto newEdges = breakCycles(edge_ab, edge_bc, edge_cd);
    // No edges are expected to be removed.
    ASSERT_EQ(newEdges.size(), 3);
}

TEST_F(GraphCycleBreakerTest, DisconnectedGraph) {
    // A.a = B.a and C.a = D.a
    auto edge_ab = addEdge(a, b);
    auto edge_cd = addEdge(c, d);

    validateCycles(/*expectedNumberOfCycles*/ 0);

    auto newEdges = breakCycles(edge_ab, edge_cd);
    // No edges are expected to be removed.
    ASSERT_EQ(newEdges.size(), 2);
}

TEST_F(GraphCycleBreakerTest, DisconnectedGraphWithCycles) {
    // A.a - B.a - C.a  - A.a
    auto edge_ab = addEdge(a, b);
    auto edge_bc = addEdge(b, c);
    auto edge_ca = addEdge(c, a);

    // D.a - E.a - F.a - D.a
    auto edge_de = addEdge(d, e);
    auto edge_ef = addEdge(e, f);
    auto edge_fd = addEdge(f, d);

    validateCycles(/*expectedNumberOfCycles*/ 2);

    auto newEdges = breakCycles(edge_ab, edge_bc, edge_ca, edge_de, edge_ef, edge_fd);
    // Two edges are expected to be removed.
    ASSERT_EQ(newEdges.size(), 4);
}

TEST_F(GraphCycleBreakerTest, IncompatiblePredicates) {
    // Define a big cycle with compatible predicates:
    // A.a = B.a and B.a = C.a and C.a = D.a and D.a = E.a and E.a = F.a and F.a = A.a
    auto edge_ab = addEdge(a, b);
    auto edge_bc = addEdge(b, c);
    auto edge_cd = addEdge(c, d);
    auto edge_de = addEdge(d, e);
    auto edge_ef = addEdge(e, f);
    auto edge_fa = addEdge(f, a);

    // Add edges with incompatible predicates. These edges would add additional cycles in the graph
    // if not their predicates:
    // C.b = A.b and D.b = A.b and E.b = A.b
    auto edge_ca = addEdge(c, a, alternativePaths);
    auto edge_da = addEdge(d, a, alternativePaths);
    auto edge_ea = addEdge(e, a, alternativePaths);

    validateCycles(/*expectedNumberOfCycles*/ 1);

    // One big loop case
    {
        auto newEdges = breakCycles(
            edge_ab, edge_bc, edge_cd, edge_de, edge_ef, edge_fa, edge_ca, edge_da, edge_ea);
        // Here's only one loop so only one edge is expected to be removed.
        ASSERT_EQ(newEdges.size(), 8);
    }

    // No loops (edge_fa is missing)
    {
        auto newEdges =
            breakCycles(edge_ab, edge_bc, edge_cd, edge_de, edge_ef, edge_ca, edge_da, edge_ea);
        // No edges is expected to be removed
        ASSERT_EQ(newEdges.size(), 8);
    }
}

TEST_F(GraphCycleBreakerTest, TwoCyclesWithDifferentPredicates) {
    // Cycle 1: A.a == B.a and B.a == C.a and C.a == D.a and D.a == A.a
    auto edge_ab = addEdge(a, b);
    auto edge_bc = addEdge(b, c);
    auto edge_cd = addEdge(c, d);
    auto edge_da = addEdge(d, a);

    // Cycle 2: D.b == E.b and E.b == F.b and F.b == D.b
    auto edge_de = addEdge(d, e, alternativePaths);
    auto edge_ef = addEdge(e, f, alternativePaths);
    auto edge_fd = addEdge(f, d, alternativePaths);

    // Standalone edge.
    // Because of the predicates there it doesn't form a big cycle: A - B - C - D - E - F - A
    auto edge_fa = addEdge(f, a, p1, p2);

    validateCycles(/*expectedNumberOfCycles*/ 2);

    auto newEdges =
        breakCycles(edge_ab, edge_bc, edge_cd, edge_da, edge_de, edge_ef, edge_fd, edge_fa);
    // Two loops
    ASSERT_EQ(newEdges.size(), 6);
}

TEST_F(GraphCycleBreakerTest, TwoCyclesWithASharedEdge) {
    // Cycle 1: A.a == B.a and B.a == C.a and C.a == D.a and D.a == A.a
    auto edge_ab = addEdge(a, b);
    auto edge_bc = addEdge(b, c);
    auto edge_cd = addEdge(c, d);
    auto edge_da = addEdge(d, a);

    // Cycle 2: A.b == D.b and D.b == E.b and E.b == F.b and F.b == A.b
    auto edge_ad = addEdge(a, d, alternativePaths);
    auto edge_de = addEdge(d, e, alternativePaths);
    auto edge_ef = addEdge(e, f, alternativePaths);
    auto edge_fa = addEdge(f, a, alternativePaths);

    validateCycles(/*expectedNumberOfCycles*/ 2);

    // The shared edge
    ASSERT_EQ(edge_da, edge_ad);

    auto newEdges =
        breakCycles(edge_ab, edge_bc, edge_cd, edge_da, edge_ad, edge_de, edge_ef, edge_fa);

    ASSERT_EQ(newEdges.size(), 6);
}

TEST_F(GraphCycleBreakerTest, CliqueOf4) {
    // Define a big cycle with compatible predicates:
    // A.a = B.a and B.a = C.a and C.a = D.a and D.a = E.a and E.a = F.a and C.a = A.a and D.a = A.a
    // and D.a = B.a.
    // A.a, B.a, C.a, D.a form a clique.
    // 7 loops in the clique:
    // * 001111: A - B - C - D - A
    // * 010011: A - B - C - A
    // * 011100: A - C - D - A
    // * 100110: B - C - D - B
    // * 101001: A - B - D - A
    // * 110101: A - B - D - C - A
    // * 111010: A - D - B - C - A
    auto edge_ab = addEdge(a, b);
    auto edge_bc = addEdge(b, c);
    auto edge_cd = addEdge(c, d);
    auto edge_da = addEdge(d, a);
    auto edge_ca = addEdge(c, a);
    auto edge_db = addEdge(d, b);
    auto edge_de = addEdge(d, e);
    auto edge_ef = addEdge(e, f);

    validateCycles(/*expectedNumberOfCycles*/ cyclesInClique(4));

    auto newEdges =
        breakCycles(edge_ab, edge_bc, edge_cd, edge_de, edge_ef, edge_ca, edge_da, edge_db);
    ASSERT_EQ(newEdges.size(), 5);
}

TEST_F(GraphCycleBreakerTest, TwoCliquesOf4) {
    addEdge(a, b);
    addEdge(b, c);
    addEdge(c, d);
    addEdge(d, a);
    addEdge(c, a);
    addEdge(d, b);
    addEdge(d, e);
    addEdge(e, f);

    addEdge(a, b, alternativePaths);
    addEdge(b, c, alternativePaths);
    addEdge(c, d, alternativePaths);
    addEdge(d, a, alternativePaths);
    addEdge(c, a, alternativePaths);
    addEdge(d, b, alternativePaths);
    addEdge(d, e, alternativePaths);
    addEdge(e, f, alternativePaths);

    validateCycles(/*expectedNumberOfCycles*/ 2 * cyclesInClique(4));
}

TEST_F(GraphCycleBreakerTest, CliqueOf6) {
    std::vector<NodeId> nodes{a, b, c, d, e, f};

    for (auto left : nodes) {
        for (auto right : nodes) {
            if (left < right) {
                addEdge(left, right);
            }
        }
    }

    validateCycles(/*expectedNumberOfCycles*/ cyclesInClique(6));
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
