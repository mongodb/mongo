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

#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"

#include "mongo/db/exec/classic/batched_delete_stage.h"
#include "mongo/db/exec/classic/count.h"
#include "mongo/db/exec/classic/projection.h"
#include "mongo/db/exec/classic/spool.h"
#include "mongo/db/exec/classic/timeseries_modify.h"
#include "mongo/db/exec/classic/timeseries_upsert.h"
#include "mongo/db/exec/classic/upsert_stage.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy_impl.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"

namespace mongo::classic_runtime_planner {

ClassicPlannerInterface::ClassicPlannerInterface(PlannerData plannerData)
    : ClassicPlannerInterface(std::move(plannerData), PlanExplainerData{}) {}

ClassicPlannerInterface::ClassicPlannerInterface(PlannerData plannerData,
                                                 PlanExplainerData explainData)
    : _plannerData(std::move(plannerData)), _planExplainerData(std::move(explainData)) {
    if (collections().hasMainCollection()) {
        _nss = collections().getMainCollection()->ns();
    } else {
        invariant(cq());
        const auto nssOrUuid = cq()->getFindCommandRequest().getNamespaceOrUUID();
        _nss = nssOrUuid.isNamespaceString() ? nssOrUuid.nss() : NamespaceString::kEmpty;
    }
}

OperationContext* ClassicPlannerInterface::opCtx() {
    return _plannerData.opCtx;
}

CanonicalQuery* ClassicPlannerInterface::cq() {
    return _plannerData.cq;
}

const MultipleCollectionAccessor& ClassicPlannerInterface::collections() const {
    return _plannerData.collections;
}

PlanYieldPolicy::YieldPolicy ClassicPlannerInterface::yieldPolicy() const {
    return collections().hasMainCollection() ? _plannerData.yieldPolicy
                                             : PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY;
}

const QueryPlannerParams& ClassicPlannerInterface::plannerParams() {
    return *_plannerData.plannerParams;
}

size_t ClassicPlannerInterface::plannerOptions() const {
    return _plannerData.plannerParams->mainCollectionInfo.options;
}

boost::optional<size_t> ClassicPlannerInterface::cachedPlanHash() const {
    return _plannerData.cachedPlanHash;
}

WorkingSet* ClassicPlannerInterface::ws() const {
    return _plannerData.workingSet.get();
}

SavedExecState ClassicPlannerInterface::extractExecState() && {
    return {.workingSet = std::move(_plannerData.workingSet), .root = std::move(_root)};
}

void ClassicPlannerInterface::addDeleteStage(ParsedDelete* parsedDelete,
                                             projection_ast::Projection* projection,
                                             std::unique_ptr<DeleteStageParams> deleteStageParams) {
    invariant(_state == kNotInitialized);
    invariant(collections().hasMainCollection());
    const auto& coll = collections().getMainCollectionAcquisition();
    const auto& collectionPtr = coll.getCollectionPtr();

    // TODO (SERVER-64506): support change streams' pre- and post-images.
    // TODO (SERVER-66079): allow batched deletions in the config.* namespace.
    const bool batchDelete = gBatchUserMultiDeletes.load() &&
        (shard_role_details::getRecoveryUnit(opCtx())->getState() ==
             RecoveryUnit::State::kInactive ||
         shard_role_details::getRecoveryUnit(opCtx())->getState() ==
             RecoveryUnit::State::kActiveNotInUnitOfWork) &&
        !opCtx()->inMultiDocumentTransaction() && !opCtx()->isRetryableWrite() &&
        !collectionPtr->isChangeStreamPreAndPostImagesEnabled() &&
        !collectionPtr->ns().isConfigDB() && deleteStageParams->isMulti &&
        !deleteStageParams->fromMigrate && !deleteStageParams->returnDeleted &&
        deleteStageParams->sort.isEmpty() && !deleteStageParams->numStatsForDoc;

    if (parsedDelete->isEligibleForArbitraryTimeseriesDelete()) {
        // Checks if the delete is on a time-series collection and cannot run on bucket
        // documents directly.
        _root = std::make_unique<TimeseriesModifyStage>(
            cq()->getExpCtxRaw(),
            TimeseriesModifyParams(deleteStageParams.get()),
            ws(),
            std::move(_root),
            coll,
            timeseries::BucketUnpacker(*collectionPtr->getTimeseriesOptions()),
            parsedDelete->releaseResidualExpr());
    } else if (batchDelete) {
        _root = std::make_unique<BatchedDeleteStage>(cq()->getExpCtxRaw(),
                                                     std::move(deleteStageParams),
                                                     std::make_unique<BatchedDeleteStageParams>(),
                                                     ws(),
                                                     coll,
                                                     _root.release());
    } else {
        _root = std::make_unique<DeleteStage>(
            cq()->getExpCtxRaw(), std::move(deleteStageParams), ws(), coll, _root.release());
    }

    if (projection) {
        _root = std::make_unique<ProjectionStageDefault>(cq()->getExpCtx(),
                                                         parsedDelete->getRequest()->getProj(),
                                                         projection,
                                                         ws(),
                                                         std::move(_root));
    }
}

void ClassicPlannerInterface::addUpdateStage(CanonicalUpdate* canonicalUpdate,
                                             projection_ast::Projection* projection,
                                             UpdateStageParams updateStageParams) {
    invariant(_state == kNotInitialized);
    invariant(collections().hasMainCollection());
    const auto& request = canonicalUpdate->getRequest();
    const bool isUpsert = updateStageParams.request->isUpsert();
    const auto timeseriesOptions = collections().getMainCollection()->getTimeseriesOptions();
    if (canonicalUpdate->isEligibleForArbitraryTimeseriesUpdate()) {
        if (request->isMulti()) {
            // If this is a multi-update, we need to spool the data before beginning to apply
            // updates, in order to avoid the Halloween problem.
            _root = std::make_unique<SpoolStage>(cq()->getExpCtxRaw(), ws(), std::move(_root));
        }
        if (isUpsert) {
            _root = std::make_unique<TimeseriesUpsertStage>(
                cq()->getExpCtxRaw(),
                TimeseriesModifyParams(&updateStageParams),
                ws(),
                std::move(_root),
                collections().getMainCollectionAcquisition(),
                timeseries::BucketUnpacker(*timeseriesOptions),
                canonicalUpdate->releaseResidualExpr(),
                canonicalUpdate->releaseOriginalExpr(),
                *request);
        } else {
            _root = std::make_unique<TimeseriesModifyStage>(
                cq()->getExpCtxRaw(),
                TimeseriesModifyParams(&updateStageParams),
                ws(),
                std::move(_root),
                collections().getMainCollectionAcquisition(),
                timeseries::BucketUnpacker(*timeseriesOptions),
                canonicalUpdate->releaseResidualExpr(),
                canonicalUpdate->releaseOriginalExpr());
        }
    } else if (isUpsert) {
        _root = std::make_unique<UpsertStage>(cq()->getExpCtxRaw(),
                                              updateStageParams,
                                              ws(),
                                              collections().getMainCollectionAcquisition(),
                                              _root.release());
    } else {
        _root = std::make_unique<UpdateStage>(cq()->getExpCtxRaw(),
                                              updateStageParams,
                                              ws(),
                                              collections().getMainCollectionAcquisition(),
                                              _root.release());
    }

    if (projection) {
        _root = std::make_unique<ProjectionStageDefault>(
            cq()->getExpCtx(), request->getProj(), projection, ws(), std::move(_root));
    }
}


void ClassicPlannerInterface::addCountStage(long long limit, long long skip) {
    invariant(_state == kNotInitialized);
    // Make a CountStage to be the new root.
    _root = std::make_unique<CountStage>(cq()->getExpCtxRaw(), limit, skip, ws(), _root.release());
}

Status ClassicPlannerInterface::plan() {
    auto classicTrialPolicy = makeClassicYieldPolicy(opCtx(), _nss, _root.get(), yieldPolicy());
    if (auto status = doPlan(classicTrialPolicy.get()); !status.isOK()) {
        return status;
    }

    _state = kInitialized;
    return Status::OK();
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> ClassicPlannerInterface::makeExecutor(
    std::unique_ptr<CanonicalQuery> canonicalQuery) {
    invariant(_state == kInitialized);
    if (cq()->getExplain().has_value()) {
        bool isCountQuery = getRoot()->stageType() == STAGE_COUNT;
        // This loop will not run if pure multiplanning is configured (featureFlagCostBasedRanker:
        // false) since rejected solutions in that case live in the multiplanner itself.
        for (auto&& solutionWithPlanStage : _planExplainerData.rejectedPlansWithStages) {
            if (!solutionWithPlanStage.planStage) {
                // If planStage is not already built, build it. This will be the case for CBR
                // rejected plans that are not multi-planned.
                auto execTree = buildExecutableTree(*solutionWithPlanStage.solution);
                solutionWithPlanStage.planStage = std::move(execTree);
            }
            if (isCountQuery) {
                tassert(11777400,
                        "Expected rejected plan to not have CountStage as root",
                        solutionWithPlanStage.planStage->stageType() != STAGE_COUNT);

                // Wrap the rejected plan's root stage in a CountStage to reflect the actual
                // execution.
                solutionWithPlanStage.planStage =
                    std::make_unique<CountStage>(cq()->getExpCtxRaw(),
                                                 static_cast<CountStage*>(getRoot())->getLimit(),
                                                 static_cast<CountStage*>(getRoot())->getSkip(),
                                                 ws(),
                                                 solutionWithPlanStage.planStage.release());
            }
        }
        for (auto& mapping : _planStageQsnMap) {
            _planExplainerData.planStageQsnMap.emplace(std::move(mapping));
        }
    }
    _state = kDisposed;

    return uassertStatusOK(plan_executor_factory::make(opCtx(),
                                                       std::move(_plannerData.workingSet),
                                                       std::move(_root),
                                                       extractQuerySolution(),
                                                       std::move(canonicalQuery),
                                                       cq()->getExpCtx(),
                                                       collections().getMainCollectionAcquisition(),
                                                       plannerOptions(),
                                                       std::move(_nss),
                                                       yieldPolicy(),
                                                       cachedPlanHash(),
                                                       std::move(_planExplainerData)));
}

std::unique_ptr<PlanStage> ClassicPlannerInterface::buildExecutableTree(const QuerySolution& qs) {
    return stage_builder::buildClassicExecutableTree(
        opCtx(),
        collections().getMainCollectionPtrOrAcquisition(),
        *cq(),
        qs,
        ws(),
        &_planStageQsnMap);
}

PlanStage* ClassicPlannerInterface::getRoot() const {
    return _root.get();
}

void ClassicPlannerInterface::setRoot(std::unique_ptr<PlanStage> root) {
    _root = std::move(root);
}
}  // namespace mongo::classic_runtime_planner
