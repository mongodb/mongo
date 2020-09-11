/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/stage_builder_util.h"

#include "mongo/db/query/classic_stage_builder.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/sbe_stage_builder.h"

namespace mongo::stage_builder {
std::unique_ptr<PlanStage> buildClassicExecutableTree(OperationContext* opCtx,
                                                      const Collection* collection,
                                                      const CanonicalQuery& cq,
                                                      const QuerySolution& solution,
                                                      WorkingSet* ws) {
    // Only QuerySolutions derived from queries parsed with context, or QuerySolutions derived from
    // queries that disallow extensions, can be properly executed. If the query does not have
    // $text/$where context (and $text/$where are allowed), then no attempt should be made to
    // execute the query.
    invariant(!cq.canHaveNoopMatchNodes());
    invariant(solution.root());
    invariant(ws);
    auto builder = std::make_unique<ClassicStageBuilder>(opCtx, collection, cq, solution, ws);
    return builder->build(solution.root());
}

std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData>
buildSlotBasedExecutableTree(OperationContext* opCtx,
                             const Collection* collection,
                             const CanonicalQuery& cq,
                             const QuerySolution& solution,
                             PlanYieldPolicy* yieldPolicy,
                             bool needsTrialRunProgressTracker) {
    // Only QuerySolutions derived from queries parsed with context, or QuerySolutions derived from
    // queries that disallow extensions, can be properly executed. If the query does not have
    // $text/$where context (and $text/$where are allowed), then no attempt should be made to
    // execute the query.
    invariant(!cq.canHaveNoopMatchNodes());
    invariant(solution.root());

    auto sbeYieldPolicy = dynamic_cast<PlanYieldPolicySBE*>(yieldPolicy);
    invariant(sbeYieldPolicy);

    auto builder = std::make_unique<SlotBasedStageBuilder>(
        opCtx, collection, cq, solution, sbeYieldPolicy, needsTrialRunProgressTracker);
    auto root = builder->build(solution.root());
    auto data = builder->getPlanStageData();

    // Register this plan to yield according to the configured policy.
    sbeYieldPolicy->registerPlan(root.get());

    return {std::move(root), std::move(data)};
}
}  // namespace mongo::stage_builder
