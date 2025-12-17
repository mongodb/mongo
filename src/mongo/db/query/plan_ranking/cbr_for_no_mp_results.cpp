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

#include "mongo/db/query/plan_ranking/cbr_for_no_mp_results.h"

#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_ranking/cbr_plan_ranking.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"

namespace mongo {
namespace plan_ranking {


StatusWith<QueryPlanner::PlanRankingResult> CBRForNoMPResultsStrategy::rankPlans(
    CanonicalQuery& query,
    QueryPlannerParams& plannerParams,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const MultipleCollectionAccessor& collections,
    OperationContext* opCtx,
    PlannerData plannerData) {
    auto statusWithMultiPlanSolns = QueryPlanner::plan(query, plannerParams);
    if (!statusWithMultiPlanSolns.isOK()) {
        return statusWithMultiPlanSolns.getStatus();
    }
    auto solutions = std::move(statusWithMultiPlanSolns.getValue());
    if (solutions.size() == 1) {
        // TODO SERVER-115496. Make sure this short circuit logic is also taken to main plan_ranking
        // so it applies everywhere. Only one solution, no need to rank.
        QueryPlanner::PlanRankingResult out;
        out.solutions.push_back(std::move(solutions.front()));
        _ws = std::move(plannerData.workingSet);
        return out;
    }
    auto solutionsSize = solutions.size();  // Caching the value before moving it.
    _multiPlanner.emplace(
        std::move(plannerData), std::move(solutions), QueryPlanner::PlanRankingResult{});
    ON_BLOCK_EXIT([&] { _ws = _multiPlanner->extractWorkingSet(); });
    // Cap the number of works per plan during this first trials phase so that the total works
    // across all plans does not exceed internalQueryPlanEvaluationWorks.
    auto trialsConfig = _multiPlanner->getTrialPhaseConfig();
    auto cappedTrialsConfig = MultiPlanStage::TrialPhaseConfig{
        .maxNumWorksPerPlan = internalQueryPlanEvaluationWorks.load() / solutionsSize,
        .targetNumResults = trialsConfig.targetNumResults};
    auto mpTrialsStatus = _multiPlanner->runTrials(cappedTrialsConfig);
    if (!mpTrialsStatus.isOK()) {
        return mpTrialsStatus;
    }
    auto stats = _multiPlanner->getSpecificStats();
    // If no plan has produced any results (absolutely zero productivity) during the trials phase
    // and also multiplanner didn't exit early (EOF), use CBR to rank the plans.
    if (!stats->earlyExit && stats->numResultsFound == 0) {
        CBRPlanRankingStrategy cbrStrategy;
        plannerParams.planRankerMode = QueryPlanRankerModeEnum::kSamplingCE;
        auto result = cbrStrategy.rankPlans(opCtx, query, plannerParams, yieldPolicy, collections);
        plannerParams.planRankerMode = QueryPlanRankerModeEnum::kAutomaticCE;
        return result;
    }
    if (!stats->earlyExit) {
        // Previous multi-planning phase didn't exit early.
        // Resume it.
        auto remainingWorksPerPlan =
            trialsConfig.maxNumWorksPerPlan - cappedTrialsConfig.maxNumWorksPerPlan;
        auto status = _multiPlanner->runTrials({.maxNumWorksPerPlan = remainingWorksPerPlan,
                                                .targetNumResults = trialsConfig.targetNumResults});
        if (!status.isOK()) {
            return status;
        }
    }
    auto status = _multiPlanner->pickBestPlan();
    if (!status.isOK()) {
        return status;
    }

    QueryPlanner::PlanRankingResult out;
    auto soln = _multiPlanner->extractQuerySolution();
    tassert(11451401, "Expected multi-planner to have returned a solution!", soln);
    out.solutions.push_back(std::move(soln));
    return out;
}

std::unique_ptr<WorkingSet> CBRForNoMPResultsStrategy::extractWorkingSet() {
    tassert(11451400, "WorkingSet is not initialized", _ws);
    auto result = std::move(_ws);
    _ws = nullptr;
    return result;
}

}  // namespace plan_ranking
}  // namespace mongo
