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

#include "mongo/db/exec/classic/subplan.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_ranking/cbr_plan_ranking.h"
#include "mongo/db/query/plan_ranking/plan_ranker.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"

namespace mongo {
namespace plan_ranking {


StatusWith<PlanRankingResult> CBRForNoMPResultsStrategy::rankPlans(PlannerData& plannerData) {
    OperationContext* opCtx = plannerData.opCtx;
    CanonicalQuery& query = *plannerData.cq;
    QueryPlannerParams& plannerParams = const_cast<QueryPlannerParams&>(*plannerData.plannerParams);
    PlanYieldPolicy::YieldPolicy yieldPolicy = plannerData.yieldPolicy;
    const MultipleCollectionAccessor& collections = plannerData.collections;

    auto statusWithMultiPlanSolns = QueryPlanner::plan(query, plannerParams);
    if (!statusWithMultiPlanSolns.isOK()) {
        return statusWithMultiPlanSolns.getStatus();
    }
    auto solutions = std::move(statusWithMultiPlanSolns.getValue());

    // TODO: SERVER-115496: move the check below to wherever we enumerate plans.
    // If this is a rooted $or query and there are more than kMaxNumberOrPlans plans, use the
    // subplanner.
    if (SubplanStage::needsSubplanning(query) && solutions.size() > kMaxNumberOfOrPlans) {
        return Status(ErrorCodes::MaxNumberOfOrPlansExceeded,
                      str::stream()
                          << "exceeded " << kMaxNumberOfOrPlans << " plans. Switch to subplanner");
    }
    if (solutions.size() == 1) {
        // TODO SERVER-115496. Make sure this short circuit logic is also taken to main plan_ranking
        // so it applies everywhere. Only one solution, no need to rank.
        plan_ranking::PlanRankingResult out;
        out.solutions.push_back(std::move(solutions.front()));
        return std::move(out);
    }
    auto solutionsSize = solutions.size();  // Caching the value before moving it.
    _multiPlanner.emplace(std::move(plannerData), std::move(solutions), PlanExplainerData{});
    // Cap the number of works per plan during this first trials phase so that the total works
    // across all plans does not exceed internalQueryPlanEvaluationWorks.
    auto trialsConfig = _multiPlanner->getTrialPhaseConfig();
    auto cappedTrialsConfig = trial_period::TrialPhaseConfig{
        .maxNumWorksPerPlan = internalQueryPlanEvaluationWorks.load() / solutionsSize,
        .targetNumResults = trialsConfig.targetNumResults,
        .isCappedTrialPhase = true};
    auto mpTrialsStatus = _multiPlanner->runTrials(cappedTrialsConfig);
    if (!mpTrialsStatus.isOK()) {
        return mpTrialsStatus;
    }
    auto stats = _multiPlanner->getSpecificStats();
    // We're using CBR to pick a plan only if multiplanner did not produce any results during the
    // trials phase and did not exit early either.
    //
    // Specifically applied as follows:
    // 1. Try multiplanner first: If it produced any results during trials phase or exited early,
    //    pick best plan from it.
    // 2. Otherwise, try CBR: If CBR picks a single best plan, return that.
    // 3. Otherwise, resume multiplanner to completion and pick best plan from it.
    if (stats->earlyExit || stats->numResultsFound > 0) {
        auto remainingMultiPlannerWorksPerPlan =
            trialsConfig.maxNumWorksPerPlan - cappedTrialsConfig.maxNumWorksPerPlan;
        return resumeMultiPlannerAndPickBestPlan(
            {.maxNumWorksPerPlan = remainingMultiPlannerWorksPerPlan,
             .targetNumResults = trialsConfig.targetNumResults});
    }
    tassert(11737001,
            "Expected multi-planner to have produced zero results during trials phase",
            stats->numResultsFound == 0);

    // No plan produced any results during the trials phase.
    CBRPlanRankingStrategy cbrStrategy;
    plannerParams.planRankerMode = QueryPlanRankerModeEnum::kSamplingCE;
    auto cbrResult = cbrStrategy.rankPlans(opCtx, query, plannerParams, yieldPolicy, collections);
    plannerParams.planRankerMode = QueryPlanRankerModeEnum::kAutomaticCE;
    if (!cbrResult.isOK()) {
        return cbrResult.getStatus();
    }

    if (cbrResult.getValue().solutions.size() == 1) {
        _multiPlanner->abandonTrials();
        // TODO SERVER-117373. Only if explain is needed.
        auto resultValue = std::move(cbrResult.getValue());
        resultValue.maybeExplainData << _multiPlanner->extractExplainData();

        return std::move(resultValue);
    } else {
        // move solutions from cbrResult into maybeExplainData.rejectedPlansWithStages
        for (size_t i = 0; i < cbrResult.getValue().solutions.size(); i++) {
            // TODO SERVER-117373. Only if explain is needed.
            cbrResult.getValue().maybeExplainData->rejectedPlansWithStages.push_back(
                {std::move(cbrResult.getValue().solutions[i]), nullptr});
        }
    }

    // Previous multi-planning phase didn't exit early.
    // Resume it since CBR couldn't pick a single best plan either.
    // TODO SERVER-117488. Only resume the plan that CBR considered best and the not estimable ones.
    auto remainingMultiPlannerWorksPerPlan =
        trialsConfig.maxNumWorksPerPlan - cappedTrialsConfig.maxNumWorksPerPlan;
    auto result =
        resumeMultiPlannerAndPickBestPlan({.maxNumWorksPerPlan = remainingMultiPlannerWorksPerPlan,
                                           .targetNumResults = trialsConfig.targetNumResults});
    if (!result.isOK()) {
        return result;
    }
    // TODO SERVER-117373. Only if explain is needed.
    result.getValue().maybeExplainData << std::move(cbrResult.getValue().maybeExplainData);
    return std::move(result.getValue());
}

StatusWith<PlanRankingResult> CBRForNoMPResultsStrategy::resumeMultiPlannerAndPickBestPlan(
    const trial_period::TrialPhaseConfig& trialsConfig) {
    auto stats = _multiPlanner->getSpecificStats();

    if (!stats->earlyExit) {
        auto status = _multiPlanner->runTrials(trialsConfig);
        if (!status.isOK()) {
            return status;
        }
    }
    auto status = _multiPlanner->pickBestPlan();
    if (!status.isOK()) {
        return status;
    }

    plan_ranking::PlanRankingResult result;
    result.solutions.push_back(_multiPlanner->extractQuerySolution());
    tassert(
        11540202, "Expected multi-planner to have returned a solution!", !result.solutions.empty());

    // TODO SERVER-117373. Only if explain is needed.
    result.maybeExplainData.emplace(_multiPlanner->extractExplainData());
    result.execState = std::move(*_multiPlanner).extractExecState();
    return std::move(result);
}
}  // namespace plan_ranking
}  // namespace mongo
