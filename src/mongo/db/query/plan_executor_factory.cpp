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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_executor_factory.h"

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/pipeline/plan_executor_pipeline.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/query/plan_executor_sbe.h"
#include "mongo/logv2/log.h"

namespace mongo::plan_executor_factory {

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
    std::unique_ptr<CanonicalQuery> cq,
    std::unique_ptr<WorkingSet> ws,
    std::unique_ptr<PlanStage> rt,
    const CollectionPtr& collection,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    NamespaceString nss,
    std::unique_ptr<QuerySolution> qs) {
    auto expCtx = cq->getExpCtx();
    return make(expCtx->opCtx,
                std::move(ws),
                std::move(rt),
                std::move(qs),
                std::move(cq),
                expCtx,
                collection,
                nss,
                yieldPolicy);
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<WorkingSet> ws,
    std::unique_ptr<PlanStage> rt,
    const CollectionPtr& collection,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    NamespaceString nss,
    std::unique_ptr<QuerySolution> qs) {
    return make(expCtx->opCtx,
                std::move(ws),
                std::move(rt),
                std::move(qs),
                nullptr,
                expCtx,
                collection,
                nss,
                yieldPolicy);
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
    OperationContext* opCtx,
    std::unique_ptr<WorkingSet> ws,
    std::unique_ptr<PlanStage> rt,
    std::unique_ptr<QuerySolution> qs,
    std::unique_ptr<CanonicalQuery> cq,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const CollectionPtr& collection,
    NamespaceString nss,
    PlanYieldPolicy::YieldPolicy yieldPolicy) {

    try {
        auto execImpl = new PlanExecutorImpl(opCtx,
                                             std::move(ws),
                                             std::move(rt),
                                             std::move(qs),
                                             std::move(cq),
                                             expCtx,
                                             collection,
                                             std::move(nss),
                                             yieldPolicy);
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
    std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> root,
    const CollectionPtr& collection,
    NamespaceString nss,
    std::unique_ptr<PlanYieldPolicySBE> yieldPolicy) {

    auto&& [rootStage, data] = root;

    LOGV2_DEBUG(4822860,
                5,
                "SBE plan",
                "slots"_attr = data.debugString(),
                "stages"_attr = sbe::DebugPrinter{}.print(rootStage.get()));

    rootStage->prepare(data.ctx);

    auto exec = new PlanExecutorSBE(opCtx,
                                    std::move(cq),
                                    std::move(root),
                                    collection,
                                    std::move(nss),
                                    false,
                                    boost::none,
                                    std::move(yieldPolicy));
    return {{exec, PlanExecutor::Deleter{opCtx}}};
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> cq,
    std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> root,
    const CollectionPtr& collection,
    NamespaceString nss,
    std::queue<std::pair<BSONObj, boost::optional<RecordId>>> stash,
    std::unique_ptr<PlanYieldPolicySBE> yieldPolicy) {

    auto&& [rootStage, data] = root;

    LOGV2_DEBUG(4822861,
                5,
                "SBE plan",
                "slots"_attr = data.debugString(),
                "stages"_attr = sbe::DebugPrinter{}.print(rootStage.get()));

    auto exec = new PlanExecutorSBE(opCtx,
                                    std::move(cq),
                                    std::move(root),
                                    collection,
                                    std::move(nss),
                                    true,
                                    stash,
                                    std::move(yieldPolicy));
    return {{exec, PlanExecutor::Deleter{opCtx}}};
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> make(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    bool isChangeStream) {
    auto* opCtx = expCtx->opCtx;
    auto exec = new PlanExecutorPipeline(std::move(expCtx), std::move(pipeline), isChangeStream);
    return {exec, PlanExecutor::Deleter{opCtx}};
}

}  // namespace mongo::plan_executor_factory
