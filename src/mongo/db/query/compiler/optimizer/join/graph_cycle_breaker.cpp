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

#include <algorithm>
#include <iterator>

#include <absl/container/flat_hash_set.h>

namespace mongo::join_ordering {
namespace {
/**
 * Creates a graph with predicates as edges and fields as nodes from the given 'joinGraph'. The
 * parameter 'numNodes' representas the number of nodes in the new graph. It is the duty of the
 * caller to make sure that 'numNodes' is big enough.
 */
AdjacencyList makeAdjacencyList(const JoinGraph& joinGraph, size_t numNodes) {
    AdjacencyList adjList;
    adjList.neighbors.resize(numNodes);

    for (EdgeId edgeId = 0; edgeId != joinGraph.numEdges(); ++edgeId) {
        const auto& edge = joinGraph.getEdge(edgeId);
        for (uint16_t predIndex = 0; predIndex != static_cast<uint16_t>(edge.predicates.size());
             ++predIndex) {
            const auto& predicate = edge.predicates[predIndex];
            const PredicateId predicateId = static_cast<PredicateId>(adjList.predicates.size());

            adjList.neighbors[predicate.left].emplace_back(predicate.right, predicateId);
            adjList.neighbors[predicate.right].emplace_back(predicate.left, predicateId);
            adjList.predicates.emplace_back(edgeId, predIndex);
        }
    }

    tassert(11509300,
            "Too many predicates in thejoin graph",
            adjList.predicates.size() < kMaxNumberOfPredicates);

    return adjList;
}

/**
 * Backtracking with prunings for finding cycles in an undirected graph.
 */
struct GraphCycleFinder {
public:
    explicit GraphCycleFinder(const AdjacencyList& adjList)
        : _adjList(adjList),
          _edges(adjList.predicates.size()),
          _blocked(adjList.neighbors.size()) {}

    absl::flat_hash_set<Bitset> findCycles() {
        // Needed to handle duplicates.
        absl::flat_hash_set<Bitset> cycles;

        // This loop tries to find cycles starting and ending with every node.
        for (size_t start = 0; start != _adjList.neighbors.size(); ++start) {
            if (_adjList.neighbors[start].size() == 0) {
                continue;
            }

            // Reset the state to prepare a new search.
            _blocked.clear();
            _edges.clear();
            ;

            findCircuits(static_cast<PathId>(start), static_cast<PathId>(start), cycles);
        }

        return cycles;
    }

private:
    /**
     * Depth-first search algorithm that identifies all cycles which starts and end with 'start'
     * node.
     */
    bool findCircuits(PathId start, PathId u, absl::flat_hash_set<Bitset>& cycles) {
        bool isCycleFound = false;
        _blocked.set(u, true);

        for (const auto& [v, predId] : _adjList.neighbors[u]) {
            // Consider only nodes which >= start node, since cycles involving nodes < start were
            // discovered in earlier call of the function. We also don't want to backtrack already
            // tracked edges.
            if (v < start || _edges.test(predId)) {
                continue;
            }

            _edges.set(predId, true);
            if (v == start) {
                // Found a cycle.
                cycles.insert(_edges);
                isCycleFound = true;
            } else if (!_blocked.test(v)) {
                if (findCircuits(start, v, cycles)) {
                    isCycleFound = true;
                }
            }
            _edges.set(predId, false);
        }
        _blocked.set(u, false);
        return isCycleFound;
    }

    const AdjacencyList& _adjList;
    // Edges seen on the path.
    Bitset _edges;
    // A node is blocked if it's currently being explored.
    Bitset _blocked;
};

/**
 * Returns the largest path seen in the graph + 1.
 */
size_t getNumberOfPaths(const JoinGraph& graph) {
    boost::optional<PathId> maxPath;
    for (const auto& edge : graph.edges()) {
        for (const auto& pred : edge.predicates) {
            maxPath = std::max({maxPath.value_or(0), pred.left, pred.right});
        }
    }
    return maxPath.has_value() ? *maxPath + 1 : 0;
}

/**
 * Returns a bitset, where each bit corresponds to an edge, a bit is set if its correspondong
 * edgehas non-compound predicates (< 2).
 */
Bitset getEdgesWithSimplePredicates(const JoinGraph& graph) {
    Bitset edgesWithSimplePredicates(graph.numEdges());
    for (EdgeId edgeId = 0; edgeId != graph.numEdges(); ++edgeId) {
        edgesWithSimplePredicates.set(edgeId, graph.getEdge(edgeId).predicates.size() < 2);
    }
    return edgesWithSimplePredicates;
}
}  // namespace

JoinGraphCycles findCycles(AdjacencyList adjList) {
    GraphCycleFinder finder(adjList);
    auto cycles = finder.findCycles();

    return JoinGraphCycles{.cycles = std::vector<Bitset>{cycles.begin(), cycles.end()},
                           .predicates = std::move(adjList.predicates)};
}

GraphCycleBreaker::GraphCycleBreaker(const JoinGraph& graph,
                                     const EdgeSelectivities& edgeSelectivities,
                                     size_t numPaths)
    : _numEdges(graph.numEdges()) {
    tassert(11509301,
            "Edges selectivites are not provided for all edges",
            edgeSelectivities.size() >= _numEdges);

    if (numPaths == 0) {
        numPaths = getNumberOfPaths(graph);
    } else if constexpr (kDebugBuild) {
        tassert(
            11509302, "Incorrect number of paths provided", getNumberOfPaths(graph) <= numPaths);
    }

    // 1. Create a graph based on predicates as edges and fields (PathId) as nodes.
    auto adjList = makeAdjacencyList(graph, numPaths);
    // 2. Finds all the cycles in the graph.
    auto cycles = findCycles(std::move(adjList));

    // 3. Find edges which we would prefer to delete in order to break the cycles and store them
    // together with the cycles.
    const auto edgesWithSimplePredicates = getEdgesWithSimplePredicates(graph);
    _cycles.reserve(cycles.cycles.size());
    std::transform(
        cycles.cycles.begin(),
        cycles.cycles.end(),
        std::back_inserter(_cycles),
        [this, &cycles, &edgeSelectivities, edgesWithSimplePredicates](const Bitset& cycle) {
            auto edges = getEdgeBitset(cycle, cycles.predicates);
            auto edge = breakCycle(edges, edgeSelectivities, edgesWithSimplePredicates);
            return CycleInfo{.edges = std::move(edges), .edge = edge};
        });
    // 4. Sorting the cycles to make the cycle breaker consistent.
    std::sort(_cycles.begin(), _cycles.end());
}

std::vector<EdgeId> GraphCycleBreaker::breakCycles(std::vector<EdgeId> subgraph) {
    Bitset edgesBitset(_numEdges);
    for (auto edgeId : subgraph) {
        edgesBitset.set(edgeId);
    }

    for (const auto& cycle : _cycles) {
        // The subgraph contains the current cycle if all the cycle edges are subset of the
        // subgraph excluding already deleted nodes.
        if (cycle.edges.isSubsetOf(edgesBitset)) {
            edgesBitset.set(cycle.edge, false);
        }
    }

    auto end = std::remove_if(
        subgraph.begin(), subgraph.end(), [&](int edgeId) { return !edgesBitset.test(edgeId); });
    subgraph.erase(end, subgraph.end());

    return subgraph;
}

EdgeId GraphCycleBreaker::breakCycle(const Bitset& cycleBitset,
                                     const EdgeSelectivities& edgeSelectivities,
                                     const Bitset& edgesWithSimplePredicates) const {
    // 1. Break a cycle of 3 by removing the middle edge sorted by selectivity.
    if (cycleBitset.count() == 3) {
        absl::InlinedVector<std::pair<cost_based_ranker::SelectivityEstimate, EdgeId>, 3>
            selectivities;
        for (auto edgeId : makePopulationView(cycleBitset)) {
            selectivities.emplace_back(edgeSelectivities.at(edgeId), static_cast<EdgeId>(edgeId));
        }
        std::sort(selectivities.begin(), selectivities.end());
        return selectivities[1].second;
    }

    // 2. Or break a cycle by selecting a non-compound edge.
    if (cycleBitset.intersects(edgesWithSimplePredicates)) {
        return (cycleBitset & edgesWithSimplePredicates).findFirst();
    }

    // 3. Or break a cycle by selecting any edge.
    return cycleBitset.findFirst();
}

Bitset GraphCycleBreaker::getEdgeBitset(
    const Bitset& predicateBitset,
    const std::vector<std::pair<EdgeId, uint16_t>>& predicates) const {
    Bitset bitset{_numEdges};
    for (auto index : makePopulationView(predicateBitset)) {
        bitset.set(predicates[index].first);
    }
    return bitset;
}
}  // namespace mongo::join_ordering
