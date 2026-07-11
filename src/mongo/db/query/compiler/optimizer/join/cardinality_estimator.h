// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/optimizer/join/cardinality_estimation_types.h"
#include "mongo/db/query/compiler/optimizer/join/graph_cycle_breaker.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/join_reordering_context.h"
#include "mongo/util/modules.h"

namespace mongo::join_ordering {

using cost_based_ranker::CardinalityEstimate;
using cost_based_ranker::SelectivityEstimate;

/**
 * Contains logic necessary to do selectivity and cardinality estimation for joins.
 */
class JoinCardinalityEstimator {
public:
    JoinCardinalityEstimator(const JoinReorderingContext& ctx, EdgeSelectivities edgeSelectivities);
    virtual ~JoinCardinalityEstimator() {};

    static JoinCardinalityEstimator make(const JoinReorderingContext& ctx,
                                         const SamplingEstimatorMap& samplingEstimators);

    /**
     * Returns an estimate of the selectivity of the given 'JoinEdge' using sampling.
     */
    static cost_based_ranker::SelectivityEstimate joinPredicateSel(
        const JoinReorderingContext& ctx,
        const SamplingEstimatorMap& samplingEstimators,
        const JoinEdge& edge);

    static EdgeSelectivities estimateEdgeSelectivities(
        const JoinReorderingContext& ctx, const SamplingEstimatorMap& samplingEstimators);

    /**
     * Estimates the cardinality of a join plan over the given subset of nodes. This method
     * constructs a spanning tree from the edges in the graph induced by 'nodes', and combines the
     * edge selectivities, base table cardinalities, and single-table predicate selectivities to
     * produce an estimate. Populates `_subsetCardinalities` with the result.
     */
    virtual CardinalityEstimate getOrEstimateSubsetCardinality(const NodeSet& nodes);

    /**
     * Returns the selectivity of the given edge.
     */
    SelectivityEstimate getEdgeSelectivity(EdgeId edge) const;

protected:
    const JoinReorderingContext& _ctx;
    const EdgeSelectivities _edgeSelectivities;

    GraphCycleBreaker _cycleBreaker;

    // Populated over the course of subset enumeration.
    SubsetCardinalities _subsetCardinalities;
};
}  // namespace mongo::join_ordering
