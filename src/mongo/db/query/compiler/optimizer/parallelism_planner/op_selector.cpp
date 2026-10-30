// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/parallelism_planner/op_selector.h"

namespace mongo::parallelism_planner {

void selectPhysicalOperators(std::unique_ptr<QuerySolutionNode>& nodeSlot,
                             const ParallelismPlannerContext& ctx) {
    // All currently supported nodes can run in a parallel context. 'ctx' is unused until the first
    // operator whose choice depends on the degree of parallelism.
}

}  // namespace mongo::parallelism_planner
