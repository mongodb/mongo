// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/optimizer/join/cardinality_estimation_types.h"
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
    struct CycleInfo {
        /**
         * Cycle stored as a bitset of edges forming the cycle.
         */
        Bitset edges;

        /**
         * Edge to delete to break the cycle .
         */
        EdgeId edge;

        friend auto operator<=>(const CycleInfo&, const CycleInfo&) = default;
    };

public:
    /**
     * Initializes new instance of GraphCycleBreaker for the given 'graph'.
     * 'edgeSelectivities' is expected to have selectivities for all edges of the graph,
     *  'numPaths' is expected to be the number of resolved paths.
     */
    explicit GraphCycleBreaker(const JoinGraph& graph,
                               const EdgeSelectivities& edgeSelectivities,
                               size_t numPaths = 0);

    /**
     * Break cycles for the subgraph specified by the list of edges. It returns new subgraph without
     * cycles.
     */
    std::vector<EdgeId> breakCycles(std::vector<EdgeId> subgraph);

    size_t numCycles_forTest() const {
        return _cycles.size();
    }

private:
    EdgeId breakCycle(const Bitset& cycleBitset,
                      const EdgeSelectivities& edgeSelectivities,
                      const Bitset& edgesWithSimplePredicates) const;
    Bitset getEdgeBitset(const Bitset& predicateBitset,
                         const std::vector<std::pair<EdgeId, uint16_t>>& predicates) const;

    size_t _numEdges;
    std::vector<CycleInfo> _cycles;
};
}  // namespace mongo::join_ordering
