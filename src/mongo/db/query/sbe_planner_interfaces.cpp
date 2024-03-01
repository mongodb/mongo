/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/sbe_planner_interfaces.h"

#include "mongo/db/query/plan_executor_factory.h"

namespace mongo {

SbeRuntimePlanner::SbeRuntimePlanner(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<PlanYieldPolicySBE> yieldPolicy,
    QueryPlannerParams plannerParams,
    boost::optional<size_t> cachedPlanHash,
    std::unique_ptr<sbe::RuntimePlanner> runtimePlanner,
    std::vector<std::unique_ptr<QuerySolution>> solutions,
    std::vector<std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData>> roots,
    std::unique_ptr<RemoteCursorMap> remoteCursors,
    std::unique_ptr<RemoteExplainVector> remoteExplains)
    : _opCtx(opCtx),
      _collections(collections),
      _yieldPolicy(std::move(yieldPolicy)),
      _plannerParams(std::move(plannerParams)),
      _cachedPlanHash(cachedPlanHash),
      _remoteCursors(std::move(remoteCursors)),
      _remoteExplains(std::move(remoteExplains)) {
    // Do the runtime planning and pick the best candidate plan.
    _candidates = runtimePlanner->plan(std::move(solutions), std::move(roots));
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> SbeRuntimePlanner::makeExecutor(
    std::unique_ptr<CanonicalQuery> canonicalQuery) {
    auto nss = canonicalQuery->nss();
    return uassertStatusOK(plan_executor_factory::make(_opCtx,
                                                       std::move(canonicalQuery),
                                                       std::move(_candidates),
                                                       _collections,
                                                       _plannerParams.options,
                                                       std::move(nss),
                                                       std::move(_yieldPolicy),
                                                       std::move(_remoteCursors),
                                                       std::move(_remoteExplains),
                                                       _cachedPlanHash));
}
}  // namespace mongo
