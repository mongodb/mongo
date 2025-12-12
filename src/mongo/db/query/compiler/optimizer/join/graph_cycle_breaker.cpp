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

#include "mongo/db/query/util/bitset_util.h"

#include <absl/container/flat_hash_set.h>

namespace mongo::join_ordering {
namespace {
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

        // 'cleared*' bitsets are used to clear their correponding bitsets.
        const Bitset clearedBlocked{_adjList.neighbors.size()};
        const Bitset clearedEdges{_adjList.predicates.size()};

        // This loop tries to find cycles starting and ending with every node.
        for (size_t start = 0; start != _adjList.neighbors.size(); ++start) {
            if (_adjList.neighbors[start].size() == 0) {
                continue;
            }

            // Reset the state to prepare a new search.
            _blocked &= clearedBlocked;
            _edges &= clearedEdges;

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
            if (v < start || _edges[predId]) {
                continue;
            }

            _edges.set(predId, true);
            if (v == start) {
                // Found a cycle.
                cycles.insert(_edges);
                isCycleFound = true;
            } else if (!_blocked[v]) {
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
}  // namespace

JoinGraphCycles findCycles(AdjacencyList adjList) {
    GraphCycleFinder finder(adjList);
    auto cycles = finder.findCycles();

    return JoinGraphCycles{.cycles = std::vector<Bitset>{cycles.begin(), cycles.end()},
                           .predicates = std::move(adjList.predicates)};
}

std::vector<EdgeId> GraphCycleBreaker::breakCycles(std::vector<EdgeId> subgraph) {
    // Performs a cycle detection in undirected graph using DFS. Once a cycle is detected the
    // last edge detected edge of the cycle is removed to break the cycle.
    //
    //  clean up
    _seen.reset();
    _edgesToRemove.clear();
    std::fill(begin(_parents), end(_parents), _sentinel);

    // Calculate adjacency matrix for the subgraph.
    const auto adjMatrix = makeAdjacencyMatrix(_graph, subgraph);

    // Traverse the graph to find out cycles and add edges to delete in _edgesToRemove.
    for (NodeId nodeId = 0; nodeId != _sentinel; ++nodeId) {
        if (!_seen[nodeId]) {
            visit(nodeId, adjMatrix, subgraph);
        }
    }

    // Remove edges to break the identified cycles.
    auto end = std::remove_if(subgraph.begin(), subgraph.end(), [&](int edgeId) {
        return _edgesToRemove.contains(edgeId);
    });
    subgraph.erase(end, subgraph.end());

    return subgraph;
}

void GraphCycleBreaker::visit(NodeId nodeId,
                              const AdjacencyMatrix& matrix,
                              const std::vector<EdgeId>& edges) {
    _seen.set(nodeId);
    for (auto otherNodeId : iterable(matrix[nodeId], _graph.numNodes())) {
        if (!_seen[otherNodeId]) {
            _parents[otherNodeId] = nodeId;
            visit(otherNodeId, matrix, edges);
        } else if (_parents[nodeId] != otherNodeId) {  // check that we don't traverse the edge back
            breakCycle(/*currentNode*/ otherNodeId, /*previousNode*/ nodeId, edges);
        }
    }
}

void GraphCycleBreaker::breakCycle(NodeId currentNode,
                                   NodeId previousNode,
                                   const std::vector<EdgeId>& edges) {
    // In this naive implementation of breaking cycles we just remove the last discovered node. This
    // is the simpliest solution and doesn't break '_parents'. However, if we delete a node from the
    // middle of the cycle we should:
    // * correct the '_parents', so that if another cycle is discovered we can use '_parents' to
    // iterate over the cycle;
    // * call the 'visit' with the 'currentNode'.
    //
    // TODO SERVER-114121: We assume here's just one edge which connects these two nodes,
    // which is true now. When we start supporting edges which have multiple nodes on one
    // side the things will become more complicated.
    auto edgeId = findEdgeId(previousNode, currentNode, edges);
    tassert(11116500, "The graph edge is expected to exist", edgeId);
    _edgesToRemove.insert(*edgeId);
}

boost::optional<EdgeId> GraphCycleBreaker::findEdgeId(NodeId u,
                                                      NodeId v,
                                                      const std::vector<EdgeId>& edges) const {
    for (auto edgeId : edges) {
        const auto& edge = _graph.getEdge(edgeId);
        if ((edge.left == u && edge.right == v) || (edge.left == v && edge.right == u)) {
            return edgeId;
        }
    }
    return boost::none;
}
}  // namespace mongo::join_ordering
