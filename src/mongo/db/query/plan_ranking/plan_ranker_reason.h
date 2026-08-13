// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {

/**
 * The reason why the deciding plan ranker (see PlanSelectionStrategy in plan_selection_strategy.h,
 * to which this enum is a sibling) was chosen for a query. Recorded at the site where each strategy
 * makes its decision and carried to explain generation.
 * The names are visible in the V3 explain 'queryPlanner.rankerChoice.reason' field and must stay
 * byte-equal to the JS 'PlanRankerReason' enum in jstests/libs/query/analyze_plan.js.
 * Absence of a reason is represented by an empty boost::optional at the carrier, not by an enum
 * member.
 */
enum class PlanRankerReason {
    // A single candidate solution (or a cached plan): no ranking ran. The only valid reason for
    // chosenRanker "none"; derived at explain emission rather than recorded by a strategy.
    kSinglePlan,
    // featureFlagCostBasedRanker is off, so planning was rewritten to the multi-planner.
    kCBRFeatureFlagDisabled,
    // The internalQueryPlanRanker knob explicitly requested this ranker (multiPlanning or
    // costBased).
    kQueryPlanRankerKnob,
    // The knob requested costBased with histogramCE, but the collection lives in an internal
    // database, where histograms are never created, therefore fallback to the multi-planner.
    // TODO SERVER-133066 remove when the limitation is removed
    kHistogramCEInternalColl,
    // CBR was engaged but could not estimate the cardinality or cost of a node in some candidate
    // plan (any node the estimators reject with ErrorCodes::UnsupportedCbrNode - see
    // cardinality_estimator.cpp), so ranking fell back to multi-planning.
    kCBRInestimableNode,
    // The multi-planning trial exited early because one plan reached EOF or filled a batch
    // (MultiPlanStats::earlyExit), so the multi-planner decided and CBR was never engaged. Emitted
    // by both mixed strategies (NoMultiplanningResults, EstimateRankingEffort). A pure-MP
    // configuration reports its config provenance instead (kCBRFeatureFlagDisabled,
    // kQueryPlanRankerKnob, kHistogramCEInternalColl): the reason names why the ranker was
    // chosen, not how its trial ended.
    kMpEarlyExit,
    // Mixed/NoMultiplanningResults: the capped multi-planning trial produced results without
    // exiting early, so the multi-planner decided and CBR was never engaged.
    kMpFoundResult,
    // Mixed/NoMultiplanningResults: no plan produced a result within the capped trial budget, so
    // CBR chose the winner.
    kNoMultiplanningResults,
    // Mixed/EstimateRankingEffort: estimating the cost of finishing multi-planning itself failed
    // (a plan contains nodes the cost estimator cannot handle), so the multi-planner decided.
    // Distinct from kCBRInestimableNode: here CBR was never engaged.
    kInestimableMP,
    // Mixed/EstimateRankingEffort: finishing multi-planning was estimated cheaper than running
    // CBR.
    kMpCheaperThanCbr,
    // Mixed/EstimateRankingEffort: CBR was estimated cheaper than finishing multi-planning
    // (including the very-low-productivity fast path).
    kCbrCheaperThanMp,
};

inline std::string_view getPlanRankerReasonName(PlanRankerReason reason) {
    switch (reason) {
        case PlanRankerReason::kSinglePlan:
            return "singlePlan";
        case PlanRankerReason::kCBRFeatureFlagDisabled:
            return "cbrFeatureFlagDisabled";
        case PlanRankerReason::kQueryPlanRankerKnob:
            return "queryPlanRankerKnob";
        case PlanRankerReason::kHistogramCEInternalColl:
            return "histogramCEInternalColl";
        case PlanRankerReason::kCBRInestimableNode:
            return "cbrInestimableNode";
        case PlanRankerReason::kMpEarlyExit:
            return "mpEarlyExit";
        case PlanRankerReason::kMpFoundResult:
            return "mpFoundResult";
        case PlanRankerReason::kNoMultiplanningResults:
            return "noMultiplanningResults";
        case PlanRankerReason::kInestimableMP:
            return "inestimableMP";
        case PlanRankerReason::kMpCheaperThanCbr:
            return "mpCheaperThanCbr";
        case PlanRankerReason::kCbrCheaperThanMp:
            return "cbrCheaperThanMp";
        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace mongo
