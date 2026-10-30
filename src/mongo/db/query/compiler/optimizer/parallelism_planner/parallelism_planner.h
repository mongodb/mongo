// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/optimizer/parallelism_planner/parallelism_planner_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/util/modules.h"

namespace mongo::parallelism_planner {

/**
 * Whether 'solution' can and should be parallelized given the context 'ctx'.
 */
bool shouldParallelize(const QuerySolution& solution, const ParallelismPlannerContext& ctx);

/**
 * Builds a parallel execution plan for 'solution', updating the root node directly.
 *
 * Unconditional: the caller must have checked shouldParallelize() first. Handing it a serial
 * context or an ineligible tree is a bug and trips a tassert.
 */
void buildParallelPlan(QuerySolution& solution, const ParallelismPlannerContext& ctx);

}  // namespace mongo::parallelism_planner
