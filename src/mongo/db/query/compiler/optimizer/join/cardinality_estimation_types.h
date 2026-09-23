// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/util/named_enum.h"
#include "mongo/util/modules.h"

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
 * Tracks the origin of an NDV estimate.
 * TODO SERVER-133669: Delete this enum/ extend the NDV sources in CE instead.
 */
#define JOIN_NDV_ESTIMATE_SOURCE_NAMES(F) \
    F(kSampling, "sampling")              \
    F(kHLL, "hyperLogLog")                \
    F(kUniqueIndex, "uniqueIndex")
QUERY_UTIL_NAMED_ENUM_DEFINE(JoinNdvEstimateSource, JOIN_NDV_ESTIMATE_SOURCE_NAMES);
#undef JOIN_NDV_ESTIMATE_SOURCE_NAMES

/**
 * Stores information about the selectivity of a join edge.
 */
struct JoinEdgeSelectivityEstimate {
    // Namespace we used as the primary key.
    NamespaceString assumedPkSide;
    cost_based_ranker::CardinalityEstimate ndv;
    cost_based_ranker::SelectivityEstimate selectivity;
    JoinNdvEstimateSource source;
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
