/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
            feature_flags::gFeatureFlagGetExecutorDeferredEngineChoice.isEnabled());
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
