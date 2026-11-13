// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/parallelism_planner/parallelism_planner.h"

#include "mongo/db/query/compiler/optimizer/parallelism_planner/op_selector.h"
#include "mongo/db/query/compiler/optimizer/parallelism_planner/required_distribution.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

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

/**
 * Whether the find command has a '$natural' hint or sort. A backward scan is already excluded by
 * CollectionScanNode's eligibility.
 */
bool requestsNaturalOrder(const CanonicalQuery& cq) {
    const auto& findCommand = cq.getFindCommandRequest();
    return findCommand.getHint()[query_request_helper::kNaturalSortField] ||
        findCommand.getSort()[query_request_helper::kNaturalSortField];
}

bool shouldParallelize(const CanonicalQuery& cq,
                       const MultipleCollectionAccessor& collections,
                       bool querySettingsApplied,
                       const QuerySolution& solution) {
    const auto& expCtx = cq.getExpCtx();
    const auto& knobs = expCtx->getQueryKnobConfiguration();
    if (!knobs.isParallelQueryEnabled() || knobs.getMaxDegreeOfParallelism() <= 1) {
        return false;
    }

    // The parallel plan cannot yet track where a pipeline extension starts. A non-leading $match
    // is pushed down as an extension and would otherwise look eligible.
    if (!solution.root() || solution.hasExtension() || !isParallelEligible(*solution.root())) {
        return false;
    }

    // A '$natural' hint or sort asks for the scan order that a parallel plan would not preserve.
    // Query settings index hints may do so too (e.g. allowedIndexes: [{$natural: 1}]), so stay
    // serial whenever any were applied.
    if (requestsNaturalOrder(cq) || querySettingsApplied) {
        return false;
    }

    // The catalog gate: only parallelize if the main collection exists and its data size exceeds
    // the minimum threshold.
    const auto& mainCollection = collections.getMainCollection();
    return mainCollection &&
        mainCollection->dataSize(cq.getExpCtx()->getOperationContext()) >=
        knobs.getParallelQueryMinCollectionSizeBytes();
}

std::unique_ptr<QuerySolutionNode> buildParallelPlan(const QuerySolutionNode& serialRoot,
                                                     const ParallelismPlannerContext& ctx) {
    tassert(13623301, "buildParallelPlan() called with a serial context", ctx.maxDoP() > 1);

    auto root = serialRoot.clone();

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
    return root;
}

}  // namespace

std::unique_ptr<QuerySolution> addParallelPlan(const CanonicalQuery& cq,
                                               const MultipleCollectionAccessor& collections,
                                               bool querySettingsApplied,
                                               std::unique_ptr<QuerySolution>&& solution) {
    if (!shouldParallelize(cq, collections, querySettingsApplied, *solution)) {
        return std::move(solution);
    }
    const ParallelismPlannerContext ctx(
        cq.getExpCtx()->getQueryKnobConfiguration().getMaxDegreeOfParallelism());

    solution->setParallelRoot(buildParallelPlan(*solution->root(), ctx));

    LOGV2_DEBUG(13623400,
                3,
                "Built a parallel plan",
                "query"_attr = redact(cq.toStringShort()),
                "maxDoP"_attr = ctx.maxDoP(),
                "plan"_attr = redact(solution->parallelRoot()->toString()));
    return std::move(solution);
}

}  // namespace mongo::parallelism_planner
