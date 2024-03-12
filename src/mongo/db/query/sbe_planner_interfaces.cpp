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

#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/stage_builder_util.h"

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
    _candidates = runtimePlanner->plan(_plannerParams, std::move(solutions), std::move(roots));
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

SbeSingleSolutionPlanner::SbeSingleSolutionPlanner(
    OperationContext* opCtx,
    CanonicalQuery* cq,
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<PlanYieldPolicySBE> yieldPolicy,
    QueryPlannerParams plannerParams,
    std::unique_ptr<QuerySolution> solution,
    std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> root,
    boost::optional<size_t> cachedPlanHash,
    bool isRecoveredPinnedCacheEntry,
    bool isRecoveredFromPlanCache,
    std::unique_ptr<RemoteCursorMap> remoteCursors,
    std::unique_ptr<RemoteExplainVector> remoteExplains)
    : _opCtx(opCtx),
      _collections(collections),
      _yieldPolicy(std::move(yieldPolicy)),
      _plannerParams(std::move(plannerParams)),
      _solution(std::move(solution)),
      _root(std::move(root)),
      _cachedPlanHash(cachedPlanHash),
      _isRecoveredFromPlanCache(isRecoveredFromPlanCache),
      _remoteCursors(std::move(remoteCursors)),
      _remoteExplains(std::move(remoteExplains)) {
    auto& [stage, data] = _root;
    if (!isRecoveredPinnedCacheEntry) {
        if (!cq->cqPipeline().empty()) {
            _solution = QueryPlanner::extendWithAggPipeline(
                *cq, std::move(_solution), _plannerParams.secondaryCollectionsInfo);
            _root = stage_builder::buildSlotBasedExecutableTree(
                opCtx, collections, *cq, *(_solution), _yieldPolicy.get());
        }

        plan_cache_util::updatePlanCache(_opCtx, _collections, *cq, *_solution, *stage, data);
    }

    // Prepare the SBE tree for execution.
    stage_builder::prepareSlotBasedExecutableTree(_opCtx,
                                                  stage.get(),
                                                  &data,
                                                  *cq,
                                                  _collections,
                                                  _yieldPolicy.get(),
                                                  _isRecoveredFromPlanCache,
                                                  _remoteCursors.get());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> SbeSingleSolutionPlanner::makeExecutor(
    std::unique_ptr<CanonicalQuery> canonicalQuery) {
    auto nss = canonicalQuery->nss();
    return uassertStatusOK(plan_executor_factory::make(_opCtx,
                                                       std::move(canonicalQuery),
                                                       nullptr /*pipeline*/,
                                                       std::move(_solution),
                                                       std::move(_root),
                                                       {},
                                                       _plannerParams.options,
                                                       std::move(nss),
                                                       std::move(_yieldPolicy),
                                                       _isRecoveredFromPlanCache,
                                                       _cachedPlanHash,
                                                       false /* generatedByBonsai */,
                                                       {} /* optCounterInfo */,
                                                       std::move(_remoteCursors),
                                                       std::move(_remoteExplains)));
}
}  // namespace mongo
