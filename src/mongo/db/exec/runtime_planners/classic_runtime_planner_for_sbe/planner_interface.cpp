// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/runtime_planners/classic_runtime_planner_for_sbe/planner_interface.h"

#include "mongo/db/query/plan_ranking/plan_selection_strategy.h"

namespace mongo::classic_runtime_planner_for_sbe {

PlannerBase::PlannerBase(PlannerDataForSBE plannerData, PlanSelectionStrategy planSelectionStrategy)
    : _plannerData(std::move(plannerData)), _planSelectionStrategy(planSelectionStrategy) {}
}  // namespace mongo::classic_runtime_planner_for_sbe
