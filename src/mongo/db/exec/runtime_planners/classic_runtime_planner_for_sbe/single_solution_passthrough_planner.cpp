// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner_for_sbe/planner_interface.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::classic_runtime_planner_for_sbe {

SingleSolutionPassthroughPlanner::SingleSolutionPassthroughPlanner(
    PlannerDataForSBE plannerData,
    std::unique_ptr<QuerySolution> solution,
    boost::optional<std::string> replanReason)
    : PlannerBase(std::move(plannerData)),
      _solution(extendSolutionWithPipelineIfNeeded(std::move(solution))),
      _sbePlanAndData(prepareSbePlanAndData(*_solution, std::move(replanReason))),
      _isFromPlanCache(false) {}

SingleSolutionPassthroughPlanner::SingleSolutionPassthroughPlanner(
    PlannerDataForSBE plannerData,
    std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> sbePlanAndData)
    : PlannerBase(std::move(plannerData)),
      _solution(nullptr),
      _sbePlanAndData(std::move(sbePlanAndData)),
      _isFromPlanCache(true) {}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> SingleSolutionPassthroughPlanner::makeExecutor(
    std::unique_ptr<CanonicalQuery> canonicalQuery) {
    LOGV2_DEBUG(8523405, 5, "Using SBE single solution planner");
    return prepareSbePlanExecutor(std::move(canonicalQuery),
                                  std::move(_solution),
                                  std::move(_sbePlanAndData),
                                  _isFromPlanCache,
                                  cachedPlanHash(),
                                  nullptr /*classicRuntimePlannerStage*/);
}

std::unique_ptr<QuerySolution> SingleSolutionPassthroughPlanner::extendSolutionWithPipelineIfNeeded(
    std::unique_ptr<QuerySolution> solution) {
    // Check if the main collection does not exist. In this case the solution should not be
    // extended because we can satisfy the query with a trivial EOF plan.
    if (!collections().hasMainCollection()) {
        tassert(9235900,
                "Expected solution with a single EOF stage",
                solution->root()->getType() == StageType::STAGE_EOF);
        return solution;
    }
    return extendSolutionWithPipeline(std::move(solution));
}
}  // namespace mongo::classic_runtime_planner_for_sbe
