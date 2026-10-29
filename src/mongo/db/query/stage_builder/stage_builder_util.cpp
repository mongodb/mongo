// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/stage_builder/stage_builder_util.h"

#include "mongo/db/query/stage_builder/classic_stage_builder.h"
#include "mongo/util/assert_util.h"

namespace mongo::stage_builder {
std::unique_ptr<PlanStage> buildClassicExecutableTree(OperationContext* opCtx,
                                                      CollectionAcquisition collection,
                                                      const CanonicalQuery& cq,
                                                      const QuerySolution& solution,
                                                      WorkingSet* ws) {
    return buildClassicExecutableTree(
        opCtx, collection, cq, solution, ws, nullptr /*planStageQsnMap=*/);
}

std::unique_ptr<PlanStage> buildClassicExecutableTree(OperationContext* opCtx,
                                                      CollectionAcquisition collection,
                                                      const CanonicalQuery& cq,
                                                      const QuerySolution& solution,
                                                      WorkingSet* ws,
                                                      PlanStageToQsnMap* planStageQsnMap) {
    // Only QuerySolutions derived from queries parsed with context, or QuerySolutions derived from
    // queries that disallow extensions, can be properly executed. If the query does not have
    // $text/$where context (and $text/$where are allowed), then no attempt should be made to
    // execute the query.
    invariant(solution.root());
    invariant(ws);
    auto builder = std::make_unique<ClassicStageBuilder>(
        opCtx, std::move(collection), cq, solution, ws, planStageQsnMap);
    return builder->build(solution.root());
}
}  // namespace mongo::stage_builder
