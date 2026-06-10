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
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_ranking/cbr_for_no_mp_results.h"
#include "mongo/db/query/plan_ranking/cbr_plan_ranking.h"
#include "mongo/db/query/plan_ranking/cost_based_plan_ranking.h"
#include "mongo/db/query/plan_ranking/mp_plan_ranking.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/planner_analysis.h"
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
    auto& rankerMode = plannerParams.planRankerMode;
    auto autoStrategy = query.getExpCtx()
                            ->getQueryKnobConfiguration()
                            .getPlanRankingStrategyForAutomaticQueryPlanRankerMode();

    const bool canUseCBR = plannerParams.cbrEnabled && isClassic;

    RankingContext rctx;

    rctx.topLevelSampleFieldNames =
        ce::extractTopLevelFieldsFromMatchExpression(query.getPrimaryMatchExpression());

    // Populating the 'topLevelSampleFields' requires 2 steps:
    //  1. Extract the fields of the relevant indexes from the plan() function by passing in
    //  the pointer to 'topLevelSampleFieldNames' as an output parameter.
    //  We do this also for trivially estimable queries, as the presence of relevant multikey
    //  indices prevent us from switching to heuristicCE.
    //  2. Extract the set of top level fields from the filter, sort and project
    //  components of the CanonicalQuery. We do this only for CE methods which might use sampling.
    auto statusWithMultiPlanSolns =
        QueryPlanner::plan(query,
                           plannerParams,
                           rctx.topLevelSampleFieldNames,
                           boost::optional<bool&>(rctx.hasRelevantMultikeyIndex));
    if (!statusWithMultiPlanSolns.isOK()) {
        return statusWithMultiPlanSolns.getStatus().withContext(
            str::stream() << "error enumerating plans for query: " << query.toStringForErrorMsg()
                          << " planner returned error");
    }

    rctx.solutions = std::move(statusWithMultiPlanSolns.getValue());
    auto& solutions = rctx.solutions;

    auto getSingleSolution = [&] {
        PlanRankingResult out;
        out.solutions.push_back(std::move(solutions.front()));
        return out;
    };

    const bool isSingleSolution = solutions.size() == 1;

    if (isSingleSolution) {
        const bool isExplain = query.getExplain().has_value();
        // TODO(SERVER-118659): Remove this disjunction once we support costing count_scan
        const bool isKnownUncostableSingleQuery =
            QueryPlannerAnalysis::isCountScan(solutions[0].get());

        const bool canSkipRanking = (
            // If CBR is disabled, single solutions will never need costing/ranking.
            // The caller will independently apply multiplanning if plan caching is required.
            !canUseCBR ||
            // Must skip any strategy if the solution is a count scan. CBR cannot cost it.
            // Note, count scans are inherently single solution cases, as a count scan plan
            // is generated by an early heuristic, and replaces all other solutions.
            isKnownUncostableSingleQuery ||
            // Otherwise, can avoid wasted work by skipping costing single solutions if not
            // in explain, as there are no other solutions to rank against, and the costing
            // info will not be used or displayed.
            !isExplain);

        if (canSkipRanking) {
            return getSingleSolution();
        }
    }

    std::unique_ptr<PlanRankingStrategy> strategy = canUseCBR
        ? makeStrategy(rankerMode, autoStrategy)
        : std::make_unique<MPPlanRankingStrategy>();

    auto swRankingResult = strategy->rankPlans(plannerData, rctx);
    return swRankingResult;
}
}  // namespace plan_ranking
}  // namespace mongo
