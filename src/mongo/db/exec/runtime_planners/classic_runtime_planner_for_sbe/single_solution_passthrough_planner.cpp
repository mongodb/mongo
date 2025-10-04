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
    if (!_isFromPlanCache && useSbePlanCache()) {
        // Create a pinned plan cache entry.
        tassert(8779800, "expected 'solution' not to be null", _solution);
        plan_cache_util::updateSbePlanCacheWithPinnedEntry(opCtx(),
                                                           collections(),
                                                           *cq(),
                                                           *_solution,
                                                           *_sbePlanAndData.first,
                                                           _sbePlanAndData.second);
    }
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
