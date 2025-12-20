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

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/optimizer/join/cardinality_estimation_types.h"
#include "mongo/db/query/compiler/optimizer/join/graph_cycle_breaker.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/single_table_access.h"
#include "mongo/util/modules.h"

namespace mongo::join_ordering {
/**
 * Contains logic necessary to do selectivity and cardinality estimation for joins.
 */
class JoinCardinalityEstimator {
public:
    JoinCardinalityEstimator(const JoinReorderingContext& ctx,
                             EdgeSelectivities edgeSelectivities,
                             NodeCardinalities nodeCardinalities);

    static JoinCardinalityEstimator make(const JoinReorderingContext& ctx,
                                         const cost_based_ranker::EstimateMap& estimates,
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

    static NodeCardinalities extractNodeCardinalities(
        const JoinReorderingContext& ctx, const cost_based_ranker::EstimateMap& estimates);

    /**
     * Estimates the cardinality of a join plan over the given subset of nodes. This method
     * constructs a spanning tree from the edges in the graph induced by 'nodes', and combines the
     * edge selectivities, base table cardinalities, and single-table predicate selectivities to
     * produce an estimate. Populates `_subsetCardinalities` with the result.
     */
    cost_based_ranker::CardinalityEstimate getOrEstimateSubsetCardinality(const NodeSet& nodes);

private:
    const JoinReorderingContext& _ctx;
    const EdgeSelectivities _edgeSelectivities;
    const NodeCardinalities _nodeCardinalities;

    GraphCycleBreaker _cycleBreaker;

    // Populated over the course of subset enumeration.
    SubsetCardinalities _subsetCardinalities;
};
}  // namespace mongo::join_ordering
