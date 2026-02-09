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

#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/stage_builder/classic_stage_builder.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"

namespace mongo::exec_deferred_engine_choice {

DeferredEngineChoicePlannerInterface::DeferredEngineChoicePlannerInterface(PlannerData plannerData)
    : _plannerData(std::move(plannerData)) {
    if (collections().hasMainCollection()) {
        _nss = collections().getMainCollection()->ns();
    } else {
        tassert(11742309, "Expected non-null canonical query", cq());
        const auto nssOrUuid = cq()->getFindCommandRequest().getNamespaceOrUUID();
        _nss = nssOrUuid.isNamespaceString() ? nssOrUuid.nss() : NamespaceString::kEmpty;
    }
}

std::unique_ptr<PlanStage> DeferredEngineChoicePlannerInterface::buildExecutableTree(
    const QuerySolution& qs) {
    return stage_builder::buildClassicExecutableTree(
        opCtx(),
        collections().getMainCollectionPtrOrAcquisition(),
        *cq(),
        qs,
        ws(),
        &_planStageQsnMap);
}

std::unique_ptr<QuerySolution> extendSolutionWithPipeline(std::unique_ptr<QuerySolution> solution,
                                                          CanonicalQuery* cq,
                                                          const QueryPlannerParams* plannerParams) {
    if (cq->cqPipeline().empty()) {
        return solution;
    }
    return QueryPlanner::extendWithAggPipeline(
        *cq, std::move(solution), plannerParams->secondaryCollectionsInfo);
}


std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>
DeferredEngineChoicePlannerInterface::makeSbePlanExecutor(std::unique_ptr<CanonicalQuery> cq,
                                                          std::unique_ptr<QuerySolution> solution,
                                                          std::unique_ptr<MultiPlanStage> mps,
                                                          Pipeline* pipeline) {
    plannerParams()->setTargetSbeStageBuilder(*cq, collections());

    finalizePipelineStages(pipeline, cq.get());
    plannerParams()->fillOutSecondaryCollectionsPlannerParams(opCtx(), *cq, collections());
    solution = extendSolutionWithPipeline(std::move(solution), cq.get(), plannerParams());
    auto sbeYieldPolicy =
        PlanYieldPolicySBE::make(opCtx(), yieldPolicy(), collections(), cq->nss());
    auto sbePlanAndData = stage_builder::buildSlotBasedExecutableTree(
        opCtx(), collections(), *cq, *solution, sbeYieldPolicy.get());

    const auto* expCtx = cq->getExpCtxRaw();
    auto remoteCursors =
        expCtx->getExplain() ? nullptr : search_helpers::getSearchRemoteCursors(cq->cqPipeline());
    auto remoteExplains = expCtx->getExplain()
        ? search_helpers::getSearchRemoteExplains(expCtx, cq->cqPipeline())
        : nullptr;

    // SERVER-117566 integrate with plan cache.
    static const boost::optional<size_t> cachedPlanHash = boost::none;
    static const bool isFromPlanCache = false;
    stage_builder::prepareSlotBasedExecutableTree(opCtx(),
                                                  sbePlanAndData.first.get(),
                                                  &sbePlanAndData.second,
                                                  *cq.get(),
                                                  collections(),
                                                  sbeYieldPolicy.get(),
                                                  isFromPlanCache,
                                                  remoteCursors.get());

    auto nss = cq->nss();
    tassert(11742306,
            "Solution must be present if cachedPlanHash is present: ",
            solution != nullptr || !cachedPlanHash.has_value());
    return uassertStatusOK(plan_executor_factory::make(opCtx(),
                                                       std::move(cq),
                                                       std::move(solution),
                                                       std::move(sbePlanAndData),
                                                       collections(),
                                                       plannerOptions(),
                                                       std::move(nss),
                                                       std::move(sbeYieldPolicy),
                                                       isFromPlanCache,
                                                       cachedPlanHash,
                                                       false /*usedJoinOpt*/,
                                                       {},
                                                       std::move(remoteCursors),
                                                       std::move(remoteExplains),
                                                       std::move(mps)));
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>
DeferredEngineChoicePlannerInterface::executorFromSolution(
    bool toSbe,
    std::unique_ptr<CanonicalQuery> cq,
    std::unique_ptr<QuerySolution> querySolution,
    std::unique_ptr<MultiPlanStage> mps,
    Pipeline* pipeline) {
    // TODO SERVER-117636 implement multiplanning in new get executor.
    if (toSbe) {
        return makeSbePlanExecutor(
            std::move(cq), std::move(querySolution), std::move(mps), pipeline);
    }
    auto expCtx = cq->getExpCtx();
    auto planStage = mps ? std::move(mps) : buildExecutableTree(*querySolution);
    return uassertStatusOK(plan_executor_factory::make(opCtx(),
                                                       std::move(_plannerData.workingSet),
                                                       std::move(planStage),
                                                       std::move(querySolution),
                                                       std::move(cq),
                                                       expCtx,
                                                       collections().getMainCollectionAcquisition(),
                                                       plannerOptions(),
                                                       std::move(_nss),
                                                       yieldPolicy(),
                                                       boost::none,
                                                       PlanExplainerData{}));
}

}  // namespace mongo::exec_deferred_engine_choice
