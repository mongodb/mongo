/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/classic/multi_plan.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/runtime_planners/exec_deferred_engine_choice_runtime_planner/planner_interface.h"
#include "mongo/db/query/plan_yield_policy_impl.h"

namespace mongo::exec_deferred_engine_choice {

MultiPlanner::MultiPlanner(PlannerData plannerData,
                           std::vector<std::unique_ptr<QuerySolution>> solutions,
                           bool addingCBRChosenPlanToPlanCache,
                           boost::optional<PlanExplainerData> maybeExplainData)
    : DeferredEngineChoicePlannerInterface(std::move(plannerData)),
      _maybeExplainData(std::move(maybeExplainData)) {
    _multiplanStage = std::make_unique<MultiPlanStage>(
        cq()->getExpCtxRaw(),
        collections().getMainCollectionPtrOrAcquisition(),
        cq(),
        plan_cache_util::ClassicPlanCacheWriter{opCtx(),
                                                collections().getMainCollectionPtrOrAcquisition()},
        boost::none /*replan reason, if present, will be returned via planner params*/,
        addingCBRChosenPlanToPlanCache);
    for (auto&& solution : solutions) {
        solution->indexFilterApplied = plannerParams()->indexFiltersApplied;
        auto executableTree = buildExecutableTree(*solution);
        _multiplanStage->addPlan(std::move(solution), std::move(executableTree), ws());
    }

    auto trialPeriodYieldPolicy = makeClassicYieldPolicy(
        opCtx(), cq()->nss(), static_cast<PlanStage*>(_multiplanStage.get()), yieldPolicy());
    uassertStatusOK(_multiplanStage->runTrials(trialPeriodYieldPolicy.get()));
    uassertStatusOK(_multiplanStage->pickBestPlan());
}

const MultiPlanStats* MultiPlanner::getSpecificStats() const {
    return static_cast<const MultiPlanStats*>(_multiplanStage->getSpecificStats());
}

PlanRankingResult MultiPlanner::extractPlanRankingResult() {
    tassert(11974300,
            "Expected `extractPlanRankingResult` to only be called with get executor deferred "
            "feature flag enabled.",
            feature_flags::gFeatureFlagGetExecutorDeferredEngineChoice.isEnabled());
    auto querySolution = _multiplanStage->extractBestSolution();

    if (!_maybeExplainData.has_value()) {
        _maybeExplainData.emplace();
    }
    _maybeExplainData->planStageQsnMap.insert(std::make_move_iterator(_planStageQsnMap.begin()),
                                              std::make_move_iterator(_planStageQsnMap.end()));

    return PlanRankingResult{.solutions = makeQsnResult(std::move(querySolution)),
                             .maybeExplainData = std::move(_maybeExplainData),
                             .execState = SavedExecState{ClassicExecState{
                                 .workingSet = extractWs(), .root = std::move(_multiplanStage)}},
                             .plannerParams = extractPlannerParams(),
                             .cachedPlanHash = cachedPlanHash()};
}
}  // namespace mongo::exec_deferred_engine_choice
