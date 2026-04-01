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


StatusWith<PlanRankingResult> CBRForNoMPResultsStrategy::rankPlans(PlannerData& plannerData) {
    OperationContext* opCtx = plannerData.opCtx;
    CanonicalQuery& query = *plannerData.cq;
    QueryPlannerParams& plannerParams = *plannerData.plannerParams;
    PlanYieldPolicy::YieldPolicy yieldPolicy = plannerData.yieldPolicy;
    const MultipleCollectionAccessor& collections = plannerData.collections;

    auto statusWithMultiPlanSolns = QueryPlanner::plan(query, plannerParams);
    if (!statusWithMultiPlanSolns.isOK()) {
        return statusWithMultiPlanSolns.getStatus();
    }
    auto solutions = std::move(statusWithMultiPlanSolns.getValue());

    if (solutions.size() == 1) {
        // TODO SERVER-115496. Make sure this short circuit logic is also taken to main plan_ranking
        // so it applies everywhere. Only one solution, no need to rank.
        PlanRankingResult out;
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
    bool isExplain = query.getExplain().has_value();

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
        // The best plan, once chosen by the multiplanner, will be inserted into the plan cache by
        // the multiplanner here.
        return resumeMultiPlannerAndPickBestPlan(
            {.maxNumWorksPerPlan = remainingMultiPlannerWorksPerPlan,
             .targetNumResults = trialsConfig.targetNumResults},
            isExplain);
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
        auto resultValue = std::move(cbrResult.getValue());

        // Stop collecting MP metrics because at this point CBR has already chosen the winning plan.
        _multiPlanner->stopCollectingMetrics();

        // TODO(SERVER-104684): Avoid abandoning the backup plan.
        _multiPlanner->abandonTrialsExceptHashes({resultValue.solutions[0]->hash()});
        auto remainingMultiPlannerWorksPerPlan =
            trialsConfig.maxNumWorksPerPlan - cappedTrialsConfig.maxNumWorksPerPlan;
        auto status =
            _multiPlanner->runTrials({.maxNumWorksPerPlan = remainingMultiPlannerWorksPerPlan,
                                      .targetNumResults = trialsConfig.targetNumResults});
        if (!status.isOK()) {
            return status;
        }
        // This call will put the CBR-chosen plan into the plan cache. Because we ran additional
        // trials above, we are caching the plan with the same number of works as if
        // multiplanning had picked the plan. Calling 'pickBestPlan' here also ensures that later
        // calls to 'doWork' function correctly.
        status = _multiPlanner->pickBestPlan();
        if (!status.isOK()) {
            return status;
        }
        resultValue.execState = std::move(*_multiPlanner).extractExecState();

        // TODO(SERVER-121641): Set resultValue.solutions[0] to nullptr (like we do when
        // MP decides) & re-introduce the tassert in get_executor.cpp from SERVER-120784.

        return resultValue;
    }

    // CBR could not decide either (there are uncostable solutions).
    // Abandon all plans not among the ones returned by CBR.
    auto computeCBRSolutionHashes = [&]() {
        boost::container::flat_set<size_t> cbrSolutionHashes;
        std::transform(cbrResult.getValue().solutions.begin(),
                       cbrResult.getValue().solutions.end(),
                       std::inserter(cbrSolutionHashes, cbrSolutionHashes.end()),
                       [](const std::unique_ptr<QuerySolution>& sol) { return sol->hash(); });
        return cbrSolutionHashes;
    };
    _multiPlanner->abandonTrialsExceptHashes(computeCBRSolutionHashes());

    if (isExplain) {
        // Move solutions from cbrResult into maybeExplainData.rejectedPlansWithStages.
        for (size_t i = 0; i < cbrResult.getValue().solutions.size(); i++) {
            cbrResult.getValue().maybeExplainData->rejectedPlansWithStages.push_back(
                {std::move(cbrResult.getValue().solutions[i]), nullptr});
        }
    }

    // Resume trials on the remainder of the plans.
    auto remainingMultiPlannerWorksPerPlan =
        trialsConfig.maxNumWorksPerPlan - cappedTrialsConfig.maxNumWorksPerPlan;
    // The best plan, once chosen by the multiplanner, will be inserted into the plan cache by the
    // multiplanner here.
    auto result =
        resumeMultiPlannerAndPickBestPlan({.maxNumWorksPerPlan = remainingMultiPlannerWorksPerPlan,
                                           .targetNumResults = trialsConfig.targetNumResults},
                                          isExplain);
    if (!result.isOK()) {
        return result;
    }

    result.getValue().maybeExplainData << std::move(cbrResult.getValue().maybeExplainData);
    return std::move(result.getValue());
}

StatusWith<PlanRankingResult> CBRForNoMPResultsStrategy::resumeMultiPlannerAndPickBestPlan(
    const trial_period::TrialPhaseConfig& trialsConfig, bool isExplain) {
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

    PlanRankingResult result;
    result.solutions.push_back(_multiPlanner->extractQuerySolution());
    tassert(
        11540202, "Expected multi-planner to have returned a solution!", !result.solutions.empty());

    if (isExplain) {
        result.maybeExplainData.emplace(_multiPlanner->extractExplainData());
    }
    result.execState = std::move(*_multiPlanner).extractExecState();
    return std::move(result);
}
}  // namespace plan_ranking
}  // namespace mongo
