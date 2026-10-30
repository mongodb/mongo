// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/optimizer/parallelism_planner/parallelism_planner_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo::parallelism_planner {

/**
 * Post-order, bottom-up traversal that replaces each node in the tree rooted at 'root' with its
 * parallel-capable equivalent, where one is needed. 'root' may itself be replaced.
 */
void selectPhysicalOperators(std::unique_ptr<QuerySolutionNode>& root,
                             const ParallelismPlannerContext& ctx);

}  // namespace mongo::parallelism_planner
