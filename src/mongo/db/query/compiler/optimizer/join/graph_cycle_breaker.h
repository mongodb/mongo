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

#pragma once

#include "mongo/db/query/compiler/optimizer/join/adjacency_matrix.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/util/dynamic_bitset.h"
#include "mongo/util/modules.h"

#include <absl/container/inlined_vector.h>

namespace mongo::join_ordering {
using Bitset = DynamicBitset<size_t, 1>;

/**
 * A graph represented as as adjacency list and used for searching cycles of predicates in Join
 * Graph. The graph's edges correspond to Join Graph's predicates and the nodes correspond to the
 * predicate's fields represented as PathIds in Join Graph.
 */
struct AdjacencyList {
    /**
     *  PathId -> {PathId, PredicateId}
     */
    std::vector<absl::InlinedVector<std::pair<PathId, PredicateId>, 8>> neighbors;

    /**
     * PredicateId -> {EdgeId, the predicate index in the edge}
     */
    std::vector<std::pair<EdgeId, uint16_t>> predicates;
};

struct JoinGraphCycles {
    /**
     * Each bit corresponds to a predicate, a set bit indicates a predicate forms part of a cycle.
     */
    std::vector<Bitset> cycles;

    /**
     * PredicateId -> {EdgeId, the predicate index in the edge}
     */
    std::vector<std::pair<EdgeId, uint16_t>> predicates;
};

JoinGraphCycles findCycles(AdjacencyList adjList);

/**
 * GraphCycleBreaker is supposed to be created one for a Join Graph and then called for each
 * subgraph to break its cycles.
 */
class GraphCycleBreaker {
public:
    explicit GraphCycleBreaker(const JoinGraph& graph) : _graph(graph) {}

    /**
     * Break cycles for the subgraph specified by the list of edges. It returns new subgraph without
     * cycles.
     */
    std::vector<EdgeId> breakCycles(std::vector<EdgeId> subgraph);

private:
    /**
     * Implements DFS algorithm to detect graph cycles.
     */
    void visit(NodeId nodeId, const AdjacencyMatrix& matrix, const std::vector<EdgeId>& edges);

    /**
     * Breaks a cycle which is defined by the currentNode and its parent previousNode.
     */
    void breakCycle(NodeId currentNode, NodeId previousNode, const std::vector<EdgeId>& edges);

    /**
     * Find EdgeId that connects two specified nodes u and v.
     */
    boost::optional<EdgeId> findEdgeId(NodeId u, NodeId v, const std::vector<EdgeId>& edges) const;

    const JoinGraph& _graph;
    const NodeId _sentinel{kMaxNodesInJoin};
    NodeSet _seen;
    std::array<NodeId, kMaxNodesInJoin> _parents;
    absl::btree_set<EdgeId> _edgesToRemove;
};
}  // namespace mongo::join_ordering
