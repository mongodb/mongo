// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo::join_ordering {
/**
 * Tracks for each node ID the cardinality estimate (with all single-table predicates applied).
 * It's important that the key is NodeId rather than namespace, since a single namespace may be
 * present multiple times in the graph and associated with different predicates/cardinalities.
 */
using NodeCardinalities = std::vector<cost_based_ranker::CardinalityEstimate>;

/**
 * Tracks for each node ID the CBR cost of the winning single-table plan.
 */
using NodeCBRCosts = std::vector<cost_based_ranker::CostEstimate>;

/**
 * The estimated number of distinct values of the join key on one side of a join edge.
 */
struct JoinKeyNdvEstimate {
    NamespaceString nss;
    cost_based_ranker::CardinalityEstimate ndv;
};

/**
 * Stores information about the selectivity of a join edge.
 */
struct JoinEdgeSelectivityEstimate {
    JoinKeyNdvEstimate smallerSide;
    // Only present if cheaply estimable.
    boost::optional<JoinKeyNdvEstimate> largerSide;
    cost_based_ranker::SelectivityEstimate selectivity;
};

/**
 * Tracks selectivity estimates of edges indexed by their EdgeIds, as well as additional estimation
 * info for explain.
 */
using EdgeSelectivities = std::vector<JoinEdgeSelectivityEstimate>;

/**
 * Tracks for each JoinSubset (represented by a NodeSet) the estimated cardinality of the join.
 */
using SubsetCardinalities = absl::flat_hash_map<NodeSet, cost_based_ranker::CardinalityEstimate>;
}  // namespace mongo::join_ordering
