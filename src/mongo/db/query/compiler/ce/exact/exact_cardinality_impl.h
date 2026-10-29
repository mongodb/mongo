// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/db/query/compiler/ce/ce_common.h"
#include "mongo/db/query/compiler/ce/exact/exact_cardinality.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates_storage.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"

namespace mongo::ce {

using CardinalityEstimate = mongo::cost_based_ranker::CardinalityEstimate;
using CEResult = StatusWith<CardinalityEstimate>;

/**
 * This class implements bottom-up exact cardinality calculation of QuerySolutionNode plans
 * that consist of QSN nodes, MatchExpression filter nodes, and Intervals. This is meant for use
 * with the "exactCE" mode.
 *
 * There are two ways to use the class and compute exact cardinalities:
 * (a) execute the query plan and then use the execution stats to populate the estimates map, or
 * (b) take an already executed plan, and use its execution stats to populate the estimates map.
 */
class ExactCardinalityImpl : public ExactCardinalityEstimator {
public:
    ExactCardinalityImpl(const CollectionAcquisition& collection,
                         const CanonicalQuery& query,
                         OperationContext* opCtx)
        : _cq(query), _opCtx(opCtx), _coll(collection) {}

    ExactCardinalityImpl(const ExactCardinalityImpl&) = delete;
    ExactCardinalityImpl(ExactCardinalityImpl&&) = delete;
    ExactCardinalityImpl& operator=(const ExactCardinalityImpl&) = delete;
    ExactCardinalityImpl& operator=(ExactCardinalityImpl&&) = delete;

    ~ExactCardinalityImpl() override {};

    /**
     * Entry point for the exact cardinality calculation, performs it for the given plan.
     */
    CEResult calculateExactCardinality(
        const QuerySolution& plan, cost_based_ranker::EstimateMap& cardinalities) const override;

    /**
     * Calculate the exact cardinality for a plan 'execStage' that has already been run.
     */
    static CEResult calculateExactCardinality(const QuerySolution* plan,
                                              const PlanStage* execStage,
                                              cost_based_ranker::EstimateMap& cardinalities) {
        return populateCardinalities(plan->root(), execStage, cardinalities);
    }

private:
    /**
     * Helper method to populate the cardinalities map given the execution stats.
     * We also pass in the QSN as these are the keys in the estimates map.
     */
    static CEResult populateCardinalities(const QuerySolutionNode* node,
                                          const PlanStage* execStage,
                                          cost_based_ranker::EstimateMap& cardinalities);
    const CanonicalQuery& _cq;
    OperationContext* _opCtx;
    const CollectionAcquisition _coll;
};
}  // namespace mongo::ce
