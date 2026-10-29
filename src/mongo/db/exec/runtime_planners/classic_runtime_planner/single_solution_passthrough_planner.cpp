// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"
#include "mongo/db/query/plan_ranking/plan_selection_strategy.h"

namespace mongo::classic_runtime_planner {

SingleSolutionPassthroughPlanner::SingleSolutionPassthroughPlanner(
    PlannerData plannerData,
    std::unique_ptr<QuerySolution> querySolution,
    PlanExplainerData explainData,
    PlanSelectionStrategy planSelectionStrategy)
    : ClassicPlannerInterface(
          std::move(plannerData), std::move(explainData), planSelectionStrategy),
      _querySolution(std::move(querySolution)) {
    auto root = buildExecutableTree(*_querySolution);
    setRoot(std::move(root));
}

/**
 * Replace the existing working set in `plannerData`, with the provided `newWs`.
 *
 * Returns the modified PlannerData.
 */
PlannerData replaceWorkingSet(PlannerData plannerData, std::unique_ptr<WorkingSet> newWs) {
    plannerData.workingSet = std::move(newWs);
    return plannerData;
}

SingleSolutionPassthroughPlanner::SingleSolutionPassthroughPlanner(
    PlannerData plannerData,
    std::unique_ptr<QuerySolution> querySolution,
    PlanExplainerData explainData,
    ClassicExecState&& state,
    PlanSelectionStrategy planSelectionStrategy)
    : ClassicPlannerInterface(
          replaceWorkingSet(std::move(plannerData), std::move(state.workingSet)),
          std::move(explainData),
          planSelectionStrategy),
      _querySolution(std::move(querySolution)) {
    setRoot(std::move(state.root));
}

Status SingleSolutionPassthroughPlanner::doPlan(PlanYieldPolicy* planYieldPolicy) {
    // Nothing to do.
    return Status::OK();
}

std::unique_ptr<QuerySolution> SingleSolutionPassthroughPlanner::extractQuerySolution() {
    return std::move(_querySolution);
}

const QuerySolution* SingleSolutionPassthroughPlanner::querySolution() const {
    return _querySolution.get();
}
}  // namespace mongo::classic_runtime_planner
