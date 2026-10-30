// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/parallelism_planner/parallelism_planner.h"

#include "mongo/db/query/compiler/optimizer/parallelism_planner/op_selector.h"
#include "mongo/db/query/compiler/optimizer/parallelism_planner/required_distribution.h"
#include "mongo/util/assert_util.h"

namespace mongo::parallelism_planner {

namespace {

bool isParallelEligible(const QuerySolutionNode& root) {
    if (!root.isParallelEligible()) {
        return false;
    }
    for (const auto& child : root.children) {
        if (!isParallelEligible(*child)) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool shouldParallelize(const QuerySolution& solution, const ParallelismPlannerContext& ctx) {
    return ctx.maxDoP() > 1 && solution.root() && isParallelEligible(*solution.root());
}

void buildParallelPlan(QuerySolution& solution, const ParallelismPlannerContext& ctx) {
    tassert(13623301,
            "buildParallelPlan() called on a solution that should not be parallelized",
            shouldParallelize(solution, ctx));

    auto root = solution.root()->clone();

    // Phase 1: Select parallel physical operators.
    selectPhysicalOperators(root, ctx);

    // Phase 2: Annotate each node with the distribution it produces, inserting exchange operators
    // wherever a child does not satisfy its parent's requirement.
    planDistribution(*root, ctx);

    // The root must be unpartitioned, as the final query result should be gathered into a single
    // stream.
    const auto rootType = root->physicalProps.distribution.type;
    tassert(13623103,
            "a replicated plan root cannot be gathered",
            rootType != DistributionDesc::Type::Replicated);
    if (rootType != DistributionDesc::Type::Unpartitioned) {
        root = insertExchangeFor(std::move(root), RequiredDistribution::unpartitioned());
    }

    solution.setRoot(std::move(root));
}

}  // namespace mongo::parallelism_planner
