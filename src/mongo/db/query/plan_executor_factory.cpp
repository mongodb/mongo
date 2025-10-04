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

#include "mongo/db/query/plan_executor_factory.h"

#include "mongo/base/status.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/pipeline/plan_executor_pipeline.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/query/plan_executor_sbe.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/sbe_plan_ranker.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <variant>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::plan_executor_factory {

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
    std::unique_ptr<CanonicalQuery> cq,
    std::unique_ptr<WorkingSet> ws,
    std::unique_ptr<PlanStage> rootStage,
    const boost::optional<CollectionAcquisition>& collection,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    size_t plannerOptions,
    NamespaceString nss,
    std::unique_ptr<QuerySolution> qs,
    boost::optional<size_t> cachedPlanHash) {
    auto expCtx = cq->getExpCtx();
    return make(expCtx->getOperationContext(),
                std::move(ws),
                std::move(rootStage),
                std::move(qs),
                std::move(cq),
                expCtx,
                collection,
                plannerOptions,
                std::move(nss),
                yieldPolicy,
                cachedPlanHash,
                QueryPlanner::CostBasedRankerResult{},
                {} /* planStageQsnMap */,
                {} /* cbrRejectedPlanStages */);
}


StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<WorkingSet> ws,
    std::unique_ptr<PlanStage> rootStage,
    const boost::optional<CollectionAcquisition>& collection,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    size_t plannerOptions,
    NamespaceString nss,
    std::unique_ptr<QuerySolution> qs) {

    return make(expCtx->getOperationContext(),
                std::move(ws),
                std::move(rootStage),
                std::move(qs),
                nullptr,
                expCtx,
                collection,
                plannerOptions,
                std::move(nss),
                yieldPolicy,
                boost::none /* cachedPlanHash */,
                QueryPlanner::CostBasedRankerResult{},
                {} /* planStageQsnMap */,
                {} /* cbrRejectedPlanStages */);
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
    OperationContext* opCtx,
    std::unique_ptr<WorkingSet> ws,
    std::unique_ptr<PlanStage> rootStage,
    std::unique_ptr<QuerySolution> qs,
    std::unique_ptr<CanonicalQuery> cq,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::optional<CollectionAcquisition>& collection,
    size_t plannerOptions,
    NamespaceString nss,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    boost::optional<size_t> cachedPlanHash,
    QueryPlanner::CostBasedRankerResult cbrResult,
    stage_builder::PlanStageToQsnMap planStageQsnMap,
    std::vector<std::unique_ptr<PlanStage>> cbrRejectedPlanStages) {
    try {
        auto execImpl = new PlanExecutorImpl(opCtx,
                                             std::move(ws),
                                             std::move(rootStage),
                                             std::move(qs),
                                             std::move(cq),
                                             expCtx,
                                             collection,
                                             plannerOptions & QueryPlannerParams::RETURN_OWNED_DATA,
                                             std::move(nss),
                                             yieldPolicy,
                                             cachedPlanHash,
                                             std::move(cbrResult),
                                             std::move(planStageQsnMap),
                                             std::move(cbrRejectedPlanStages));
        PlanExecutor::Deleter planDeleter(opCtx);
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec(execImpl, std::move(planDeleter));
        return {std::move(exec)};
    } catch (...) {
        return {exceptionToStatus()};
    }
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> cq,
    std::unique_ptr<QuerySolution> solution,
    std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> root,
    const MultipleCollectionAccessor& collections,
    size_t plannerOptions,
    NamespaceString nss,
    std::unique_ptr<PlanYieldPolicySBE> yieldPolicy,
    bool planIsFromCache,
    boost::optional<size_t> cachedPlanHash,
    std::unique_ptr<RemoteCursorMap> remoteCursors,
    std::unique_ptr<RemoteExplainVector> remoteExplains,
    std::unique_ptr<MultiPlanStage> classicRuntimePlannerStage) {
    auto&& [rootStage, data] = root;
    LOGV2_DEBUG(4822860,
                5,
                "SBE plan",
                "slots"_attr = redact(data.debugString()),
                "stages"_attr = redact(sbe::DebugPrinter{}.print(*rootStage)));

    return {{new PlanExecutorSBE(opCtx,
                                 std::move(cq),
                                 sbe::plan_ranker::CandidatePlan{
                                     std::move(solution),
                                     std::move(rootStage),
                                     sbe::plan_ranker::CandidatePlanData{std::move(data)},
                                     false /*exitedEarly*/,
                                     Status::OK(),
                                     planIsFromCache},
                                 plannerOptions & QueryPlannerParams::RETURN_OWNED_DATA,
                                 std::move(nss),
                                 false /*isOpen*/,
                                 std::move(yieldPolicy),
                                 cachedPlanHash,
                                 std::move(remoteCursors),
                                 std::move(remoteExplains),
                                 std::move(classicRuntimePlannerStage),
                                 collections),
             PlanExecutor::Deleter{opCtx}}};
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> cq,
    sbe::plan_ranker::CandidatePlan candidate,
    const MultipleCollectionAccessor& collections,
    size_t plannerOptions,
    NamespaceString nss,
    std::unique_ptr<PlanYieldPolicySBE> yieldPolicy,
    std::unique_ptr<RemoteCursorMap> remoteCursors,
    std::unique_ptr<RemoteExplainVector> remoteExplains,
    boost::optional<size_t> cachedPlanHash) {
    LOGV2_DEBUG(4822861,
                5,
                "SBE plan",
                "slots"_attr = redact(candidate.data.stageData.debugString()),
                "stages"_attr = redact(sbe::DebugPrinter{}.print(*candidate.root)));

    return {{new PlanExecutorSBE(opCtx,
                                 std::move(cq),
                                 std::move(candidate),
                                 plannerOptions & QueryPlannerParams::RETURN_OWNED_DATA,
                                 std::move(nss),
                                 true, /*isOpen*/
                                 std::move(yieldPolicy),
                                 cachedPlanHash,
                                 std::move(remoteCursors),
                                 std::move(remoteExplains),
                                 nullptr /*classicRuntimePlannerStage*/,
                                 collections),
             PlanExecutor::Deleter{opCtx}}};
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> make(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    std::unique_ptr<Pipeline> pipeline,
    PlanExecutorPipeline::ResumableScanType resumableScanType) {
    auto* opCtx = expCtx->getOperationContext();
    auto exec = new PlanExecutorPipeline(std::move(expCtx), std::move(pipeline), resumableScanType);
    return {exec, PlanExecutor::Deleter{opCtx}};
}

}  // namespace mongo::plan_executor_factory
