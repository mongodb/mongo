// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/runtime_planners/exec_deferred_engine_choice_runtime_planner/planner_interface.h"

#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"

namespace mongo::exec_deferred_engine_choice {

DeferredEngineChoicePlannerInterface::DeferredEngineChoicePlannerInterface(PlannerData plannerData)
    : _plannerData(std::move(plannerData)) {}

std::unique_ptr<PlanStage> DeferredEngineChoicePlannerInterface::buildExecutableTree(
    const QuerySolution& qs) {
    return stage_builder::buildClassicExecutableTree(
        opCtx(),
        collections().getMainCollectionPtrOrAcquisition(),
        *cq(),
        qs,
        ws(),
        &_planStageQsnMap);
}

std::vector<std::unique_ptr<QuerySolution>> makeQsnResult(std::unique_ptr<QuerySolution> qsn) {
    std::vector<std::unique_ptr<QuerySolution>> v;
    v.push_back(std::move(qsn));
    return v;
}

PreComputedRankingResultPlanner::PreComputedRankingResultPlanner(PlannerData plannerData,
                                                                 PlanRankingResult result)
    : DeferredEngineChoicePlannerInterface(std::move(plannerData)), _result(std::move(result)) {}

PlanRankingResult PreComputedRankingResultPlanner::extractPlanRankingResult() {
    tassert(11282302,
            "Expected `extractPlanRankingResult` to only be called with get executor deferred "
            "feature flag enabled.",
            cq()->getExpCtx()->getIfrContext()->getSavedFlagValue(
                feature_flags::gFeatureFlagGetExecutorDeferredEngineChoice));
    return std::move(_result);
}

}  // namespace mongo::exec_deferred_engine_choice
