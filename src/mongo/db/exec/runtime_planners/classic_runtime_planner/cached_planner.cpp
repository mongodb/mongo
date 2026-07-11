// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"
#include "mongo/db/exec/runtime_planners/planner_types.h"

namespace mongo::classic_runtime_planner {

CachedPlanner::CachedPlanner(PlannerData plannerData,
                             std::unique_ptr<CachedSolution> cachedSolution,
                             std::unique_ptr<QuerySolution> querySolution)
    : ClassicPlannerInterface(std::move(plannerData)), _querySolution(std::move(querySolution)) {
    auto root = std::make_unique<CachedPlanStage>(cq()->getExpCtxRaw(),
                                                  collections().getMainCollectionPtrOrAcquisition(),
                                                  ws(),
                                                  cq(),
                                                  cachedSolution->decisionWorks().value(),
                                                  buildExecutableTree(*_querySolution),
                                                  cachedSolution->cachedPlan->solutionHash);
    _cachedPlanStage = root.get();
    setRoot(std::move(root));
}

Status CachedPlanner::doPlan(PlanYieldPolicy* planYieldPolicy) {
    return _cachedPlanStage->pickBestPlan(plannerParams(), planYieldPolicy);
}

std::unique_ptr<QuerySolution> CachedPlanner::extractQuerySolution() {
    return std::move(_querySolution);
}

const QuerySolution* CachedPlanner::querySolution() const {
    return _querySolution.get();
}

PlanRankingResult CachedPlanner::extractPlanRankingResult() {
    tassert(11756603,
            "Expected `extractPlanRankingResult` to only be called with get executor deferred "
            "feature flag enabled.",
            cq()->getExpCtx()->getIfrContext()->getSavedFlagValue(
                feature_flags::gFeatureFlagGetExecutorDeferredEngineChoice));
    std::vector<std::unique_ptr<QuerySolution>> solutions;
    solutions.push_back(extractQuerySolution());
    return PlanRankingResult{
        .solutions = std::move(solutions),
        .maybeExplainData = PlanExplainerData{.fromPlanCache = true},
        .execState =
            SavedExecState{ClassicExecState{.workingSet = extractWs(), .root = extractRoot()}},
        .plannerParams = extractPlannerParams(),
        .cachedPlanHash = cachedPlanHash(),
        .engineSelection = EngineChoice::kClassic,
    };
}
}  // namespace mongo::classic_runtime_planner
