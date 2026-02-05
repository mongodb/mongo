/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/query/plan_ranking/plan_ranker.h"

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_ranking/cbr_for_no_mp_results.h"
#include "mongo/db/query/plan_ranking/cbr_plan_ranking.h"
#include "mongo/db/query/plan_ranking/cost_based_plan_ranking.h"
#include "mongo/db/query/plan_ranking/mp_plan_ranking.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/query_planner_params.h"

#include <memory>
#include <utility>

namespace mongo {
namespace plan_ranking {

std::unique_ptr<PlanRankingStrategy> makeStrategy(
    QueryPlanRankerModeEnum rankerMode,
    QueryPlanRankingStrategyForAutomaticQueryPlanRankerModeEnum autoStrategy) {
    // Shorter aliases for readability.
    using RankerMode = QueryPlanRankerModeEnum;
    using AutoStrategy = QueryPlanRankingStrategyForAutomaticQueryPlanRankerModeEnum;
    switch (rankerMode) {
        case RankerMode::kSamplingCE:
        case RankerMode::kExactCE:
        case RankerMode::kHeuristicCE:
        case RankerMode::kHistogramCE: {
            return std::make_unique<CBRPlanRankingStrategy>();
        }
        case RankerMode::kAutomaticCE: {
            // TODO SERVER-111770. Finalise values and names.
            switch (autoStrategy) {
                case AutoStrategy::kCBRForNoMultiplanningResults: {
                    return std::make_unique<CBRForNoMPResultsStrategy>();
                }
                case AutoStrategy::kCBRCostBasedRankerChoice: {
                    return std::make_unique<CostBasedPlanRankingStrategy>();
                }
                case AutoStrategy::kHistogramCEWithHeuristicFallback: {
                    return std::make_unique<CBRPlanRankingStrategy>();
                }
                default:
                    MONGO_UNREACHABLE;
            }
        }
        default:
            MONGO_UNREACHABLE;
    }
}

StatusWith<PlanRankingResult> PlanRanker::rankPlans(OperationContext* opCtx,
                                                    CanonicalQuery& query,
                                                    QueryPlannerParams& plannerParams,
                                                    PlanYieldPolicy::YieldPolicy yieldPolicy,
                                                    const MultipleCollectionAccessor& collections,
                                                    PlannerData plannerData,
                                                    bool isClassic) {
    auto rankerMode = plannerParams.planRankerMode;

    const bool canUseCBR = plannerParams.cbrEnabled && isClassic;
    std::unique_ptr<PlanRankingStrategy> strategy;
    if (!canUseCBR) {
        strategy = std::make_unique<MPPlanRankingStrategy>();
    } else {
        strategy = makeStrategy(rankerMode,
                                query.getExpCtx()
                                    ->getQueryKnobConfiguration()
                                    .getPlanRankingStrategyForAutomaticQueryPlanRankerMode());
    }

    // TODO SERVER-115496. Enumerate solutions here and pass them to the ranking strategy.
    auto swRankingResult = strategy->rankPlans(plannerData);
    return swRankingResult;
}

bool delayOrSkipSubplanner(const CanonicalQuery& query,
                           const QueryPlannerParams& params,
                           bool isClassic) {
    // With CBR enabled there should be no changes to the SBE planning path.
    // TODO SERVER-117707: Decide what to do with this restriction.
    if (!isClassic) {
        return false;
    }

    if (!params.cbrEnabled || params.planRankerMode != QueryPlanRankerModeEnum::kAutomaticCE) {
        return false;
    }

    // Always use the subplanner for clustered collections to enable clustered index scans for $or
    // branches.
    // TODO SERVER-117766: Avoid subplanner for $or queries over clustered collections.
    if (params.clusteredInfo) {
        return false;
    }

    auto autoPlanRankingStrategy = query.getExpCtx()
                                       ->getQueryKnobConfiguration()
                                       .getPlanRankingStrategyForAutomaticQueryPlanRankerMode();
    switch (autoPlanRankingStrategy) {
        case QueryPlanRankingStrategyForAutomaticQueryPlanRankerModeEnum::
            kCBRForNoMultiplanningResults:
        case QueryPlanRankingStrategyForAutomaticQueryPlanRankerModeEnum::kCBRCostBasedRankerChoice:
            return true;
        case QueryPlanRankingStrategyForAutomaticQueryPlanRankerModeEnum::
            kHistogramCEWithHeuristicFallback:
            return false;
        default:
            MONGO_UNREACHABLE;
    }
}
}  // namespace plan_ranking
}  // namespace mongo
