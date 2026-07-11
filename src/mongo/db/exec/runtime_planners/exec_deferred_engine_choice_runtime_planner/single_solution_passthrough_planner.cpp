// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/runtime_planners/exec_deferred_engine_choice_runtime_planner/planner_interface.h"

namespace mongo::exec_deferred_engine_choice {

SingleSolutionPassthroughPlanner::SingleSolutionPassthroughPlanner(
    PlannerData plannerData,
    std::unique_ptr<QuerySolution> querySolution,
    boost::optional<PlanExplainerData> maybeExplainData)
    : DeferredEngineChoicePlannerInterface(std::move(plannerData)),
      _querySolution(std::move(querySolution)),
      _maybeExplainData(std::move(maybeExplainData)) {}

PlanRankingResult SingleSolutionPassthroughPlanner::extractPlanRankingResult() {
    tassert(11974302,
            "Expected `extractPlanRankingResult` to only be called with get executor deferred "
            "feature flag enabled.",
            cq()->getExpCtx()->getIfrContext()->getSavedFlagValue(
                feature_flags::gFeatureFlagGetExecutorDeferredEngineChoice));
    return PlanRankingResult{.solutions = makeQsnResult(std::move(_querySolution)),
                             .maybeExplainData = std::move(_maybeExplainData),
                             .plannerParams = extractPlannerParams(),
                             .cachedPlanHash = cachedPlanHash()};
}
}  // namespace mongo::exec_deferred_engine_choice
