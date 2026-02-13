/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/runtime_planners/exec_deferred_engine_choice_runtime_planner/planner_interface.h"
#include "mongo/db/query/engine_selection.h"

namespace mongo::exec_deferred_engine_choice {

SingleSolutionPassthroughPlanner::SingleSolutionPassthroughPlanner(
    PlannerData plannerData, std::unique_ptr<QuerySolution> querySolution)
    : DeferredEngineChoicePlannerInterface(std::move(plannerData)),
      _querySolution(std::move(querySolution)) {}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> SingleSolutionPassthroughPlanner::makeExecutor(
    std::unique_ptr<CanonicalQuery> canonicalQuery, Pipeline* pipeline) {
    bool needsMerge = canonicalQuery->getExpCtx()->getNeedsMerge();
    auto engine = chooseEngine(
        opCtx(),
        collections(),
        canonicalQuery.get(),
        pipeline,
        needsMerge,
        std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForPushDownStagesDecision{
            .opCtx = opCtx(),
            .canonicalQuery = *cq(),
            .collections = collections(),
            .plannerOptions = plannerOptions(),
        }),
        _querySolution.get());
    return executorFromSolution(
        engine, std::move(canonicalQuery), std::move(_querySolution), nullptr, pipeline);
}
}  // namespace mongo::exec_deferred_engine_choice
