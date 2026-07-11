// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
            cq()->getExpCtx()->getIfrContext()->getSavedFlagValue(
                feature_flags::gFeatureFlagGetExecutorDeferredEngineChoice));
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
