// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/optimizer/join/cardinality_estimator.h"
#include "mongo/db/query/compiler/optimizer/join/hint.h"
#include "mongo/db/query/compiler/optimizer/join/join_cost_estimator.h"
#include "mongo/db/query/compiler/optimizer/join/join_plan.h"
#include "mongo/db/query/compiler/optimizer/join/join_reordering_context.h"
#include "mongo/util/modules.h"

namespace mongo::join_ordering {

/**
 * Context containing all the state for the bottom-up dynamic programming join plan enumeration
 * algorithm.
 */
class PlanEnumeratorContext {
public:
    PlanEnumeratorContext(const JoinReorderingContext& ctx,
                          JoinCardinalityEstimator* estimator,
                          JoinCostEstimator* coster,
                          EnumerationStrategy strategy)
        : _ctx{ctx},
          _estimator(std::move(estimator)),
          _coster(std::move(coster)),
          _strategy(std::move(strategy)) {}

    // Delete copy and move operations to ensure concrete ownership.
    PlanEnumeratorContext(const PlanEnumeratorContext&) = delete;
    PlanEnumeratorContext& operator=(const PlanEnumeratorContext&) = delete;
    PlanEnumeratorContext(PlanEnumeratorContext&&) = delete;
    PlanEnumeratorContext& operator=(PlanEnumeratorContext&&) = delete;

    /**
     * Return all 'JoinSubsets' of size level+1.
     */
    const std::vector<JoinSubset>& getSubsets(int level);

    /**
     * Enumerates all join subsets in bottom-up fashion.
     */
    void enumerateJoinSubsets();

    /**
     * Did the plan enumerator find a plan?
     */
    bool enumerationSuccessful() const {
        return _joinSubsets[_joinSubsets.size() - 1].size() == 1 &&
            !_joinSubsets[_joinSubsets.size() - 1][0].plans.empty();
    }

    JoinPlanNodeId getBestFinalPlan() const {
        return finalSubset().bestPlan();
    }

    /**
     * Returns all plan node ids for plans enumerated in the last subset other than the best plan.
     * Used for explain().
     */
    std::vector<JoinPlanNodeId> getRejectedFinalPlans() const;

    const JoinPlanNodeRegistry& registry() const {
        return _registry;
    }

    JoinCardinalityEstimator* getJoinCardinalityEstimator() const {
        return _estimator;
    }

    JoinCostEstimator* getJoinCostEstimator() const {
        return _coster;
    }

    /**
     * Used for testing & debugging.
     */
    std::string toString() const;
    const EnumerationStrategy& getStrategy() const {
        return _strategy;
    }

private:
    const JoinSubset& finalSubset() const {
        tassert(
            11336904, "Expected subsets to have already been enumerated", enumerationSuccessful());
        return _joinSubsets[_joinSubsets.size() - 1][0];
    }

    /**
     * Enumerate plans by constructing possible joins between the 'left' and 'right' subsets and
     * outputting those plans in 'cur'. Note that 'left' and 'right' must be disjoint, and their
     * union must produce 'cur'.
     */
    void enumerateJoinPlans(const JoinSubset& left, const JoinSubset& right, JoinSubset& cur);

    /**
     * Helpers for adding a join plan to subset 'cur', constructed using the specified join 'method'
     * connecting the best plans from the provided subsets in 'CHEAPEST' enuemration mode, and every
     * pair of plans in 'ALL' plans enumeration mode.
     */
    void addJoinPlan(JoinMethod method,
                     const JoinSubset& left,
                     const JoinSubset& right,
                     const std::vector<EdgeId>& edges,
                     JoinSubset& cur);
    void enumerateAllJoinPlans(JoinMethod method,
                               const JoinSubset& left,
                               const JoinSubset& right,
                               const std::vector<EdgeId>& edges,
                               JoinSubset& subset);
    void enumerateCheapestJoinPlan(JoinMethod method,
                                   const JoinSubset& left,
                                   const JoinSubset& right,
                                   const std::vector<EdgeId>& edges,
                                   JoinSubset& subset);
    void enumerateHintedPlan(JoinMethod method,
                             const JoinSubset& left,
                             const JoinSubset& right,
                             const std::vector<EdgeId>& edges,
                             JoinSubset& subset);

    /**
     * Helper for enumerating an INLJ plan with the specified left subtree & generating an index
     * probe on the RHS for the subset 'right'.
     */
    void enumerateINLJPlan(EdgeId edge,
                           JoinPlanNodeId leftPlan,
                           const JoinSubset& right,
                           JoinSubset& subset);

    /**
     * Helper to enumerate an NLJ/HJ join plan combining the subtrees identified by 'leftPlan' &
     * 'rightPlan'.
     */
    void enumerateJoinPlan(JoinMethod method,
                           JoinPlanNodeId leftPlan,
                           JoinPlanNodeId rightPlan,
                           JoinSubset& subset);

    /**
     * Determines based on the shape of the tree obtained by joining the best plans on each side if
     * we would retain the tree shape specified by 'type' and the plan is valid for the given join
     * 'method'.
     */
    bool canPlanBeEnumerated(JoinMethod method,
                             const JoinSubset& left,
                             const JoinSubset& right,
                             const JoinSubset& subset);

    void addPlanToSubset(JoinMethod method,
                         JoinPlanNodeId left,
                         JoinPlanNodeId right,
                         JoinCostEstimate cost,
                         JoinSubset& subset,
                         bool isBestPlan);

    inline bool isBestPlanSoFar(const JoinSubset& subset, const JoinCostEstimate& planCost) const {
        return !subset.hasPlans() || (planCost < _registry.getCost(subset.bestPlan()));
    }

    const JoinReorderingContext& _ctx;
    // Unowned pointers! If null, all costs are set to 0.
    JoinCardinalityEstimator* _estimator;
    JoinCostEstimator* _coster;
    EnumerationStrategy _strategy;

    // Variable tracking current enumeration mode during enumeration.
    SubsetLevelMode _mode{0, PlanEnumerationMode::CHEAPEST};

    // Hold intermediate results of the enumeration algorithm. The index into the outer vector
    // represents the "level". The i'th level contains solutions for the optimal way to join all
    // possible subsets of size i+1.
    std::vector<std::vector<JoinSubset>> _joinSubsets;

    // Memory management for trees so we can reuse nodes.
    JoinPlanNodeRegistry _registry;
};

}  // namespace mongo::join_ordering
