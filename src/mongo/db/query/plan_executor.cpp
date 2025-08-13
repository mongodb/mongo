/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/query/plan_executor.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(planExecutorAlwaysFails);
}  // namespace

const OperationContext::Decoration<PlanExecutorShardingState> planExecutorShardingState =
    OperationContext::declareDecoration<PlanExecutorShardingState>();

std::string PlanExecutor::stateToStr(ExecState execState) {
    switch (execState) {
        case PlanExecutor::ADVANCED:
            return "ADVANCED";
        case PlanExecutor::IS_EOF:
            return "IS_EOF";
    }
    MONGO_UNREACHABLE;
}

std::string PlanExecutor::writeTypeToStr(PlanExecWriteType writeType) {
    switch (writeType) {
        case PlanExecWriteType::kUpdate:
            return "update";
        case PlanExecWriteType::kDelete:
            return "delete";
        case PlanExecWriteType::kFindAndModify:
            return "findAndModify";
    }
    MONGO_UNREACHABLE;
}

void PlanExecutor::checkFailPointPlanExecAlwaysFails() {
    if (auto scoped = planExecutorAlwaysFails.scoped(); MONGO_unlikely(scoped.isActive())) {
        if (scoped.getData().hasField("tassert") && scoped.getData().getBoolField("tassert")) {
            tasserted(9028201, "PlanExecutor hit planExecutorAlwaysFails fail point");
        }
        uasserted(4382101, "PlanExecutor hit planExecutorAlwaysFails fail point");
    }
}

size_t PlanExecutor::getNextBatch(size_t batchSize, AppendBSONObjFn append) {
    // Subclasses may override this in order to provide a more optimized loop.
    const bool hasAppendFn = static_cast<bool>(append);
    BSONObj obj;
    BSONObj* objPtr = hasAppendFn ? &obj : nullptr;

    size_t numResults = 0;

    while (numResults < batchSize) {
        if (PlanExecutor::IS_EOF == getNext(objPtr, nullptr)) {
            break;
        }

        if (hasAppendFn && !append(obj, getPostBatchResumeToken(), numResults)) {
            stashResult(obj);
            break;
        }
        numResults++;
    }

    return numResults;
}

boost::optional<BSONObj> PlanExecutor::executeWrite(PlanExecWriteType writeType) {
    BSONObj value;
    PlanExecutor::ExecState state;
    try {
        // Multi-updates and multi-deletes never return 'ADVANCED'. Therefore, running 'getNext()'
        // once will either perform a single write for multi:false statements, or will perform an
        // entire multi:true statement.
        state = getNext(&value, nullptr);
    } catch (const StorageUnavailableException&) {
        throw;
    } catch (ExceptionFor<ErrorCodes::StaleConfig>& ex) {
        // A 'StaleConfig' exception needs to be changed to an operation-fatal 'QueryPlanKilled'
        // exception for multi-updates that have already modified some documents. First, re-throw
        // the exception if we're not an update.
        if (writeType != PlanExecWriteType::kUpdate) {
            throw;
        }

        const auto updateResult = getUpdateResult();
        tassert(
            9146500,
            fmt::format(
                "An update plan should never yield after having performed an upsert; upsertId: {}",
                redact(updateResult.upsertedId.toString())),
            updateResult.upsertedId.isEmpty());
        if (updateResult.numDocsModified > 0 && !getOpCtx()->isRetryableWrite() &&
            !getOpCtx()->inMultiDocumentTransaction()) {
            // An update plan can fail with StaleConfig error after having performed some writes but
            // not completed. This can happen when the collection is moved. Routers consider
            // StaleConfig as retryable. However, it is unsafe to retry, because if the update is
            // not idempotent it would cause some documents to be updated twice. To prevent that, we
            // rewrite the error code to QueryPlanKilled, which routers won't retry on.
            ex.addContext("Update plan failed after having partially executed");
            uasserted(ErrorCodes::QueryPlanKilled, ex.reason());
        } else {
            throw;
        }
    } catch (DBException& exception) {
        auto&& explainer = getPlanExplainer();
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        auto operationName = writeTypeToStr(writeType);
        LOGV2_WARNING(7267501,
                      "Plan executor error",
                      "operation"_attr = operationName,
                      "error"_attr = exception.toStatus(),
                      "stats"_attr = redact(stats));

        exception.addContext(str::stream() << "Plan executor error during " << operationName);
        throw;
    }

    if (PlanExecutor::ADVANCED == state) {
        return {std::move(value)};
    }

    invariant(state == PlanExecutor::IS_EOF);
    return boost::none;
}

UpdateResult PlanExecutor::executeUpdate() {
    auto doc = executeWrite(PlanExecWriteType::kUpdate);
    tassert(9212600, "expected 'boost::none' return value from executeWrite()", !doc);
    return getUpdateResult();
}

long long PlanExecutor::executeDelete() {
    auto doc = executeWrite(PlanExecWriteType::kDelete);
    tassert(9212601, "expected 'boost::none' return value from executeWrite()", !doc);
    return getDeleteResult();
}

boost::optional<BSONObj> PlanExecutor::executeFindAndModify() {
    return executeWrite(PlanExecWriteType::kFindAndModify);
}

void PlanExecutor::releaseAllAcquiredResources() {
    auto opCtx = getOpCtx();
    invariant(opCtx);

    saveState();
    // Detach + reattach forces us to release all resources back to the storage engine. This is
    // currently a side-effect of how Storage Engine cursors are implemented in the plans.
    //
    // TODO SERVER-87866: See if we can remove this if saveState/restoreState actually release all
    // resources.
    detachFromOperationContext();
    reattachToOperationContext(opCtx);
}

const CollectionPtr& VariantCollectionPtrOrAcquisition::getCollectionPtr() const {
    return *visit(OverloadedVisitor{
                      [](const CollectionPtr* collectionPtr) { return collectionPtr; },
                      [](const CollectionAcquisition& collectionAcquisition) {
                          return &collectionAcquisition.getCollectionPtr();
                      },
                  },
                  _collectionPtrOrAcquisition);
}

boost::optional<ScopedCollectionFilter> VariantCollectionPtrOrAcquisition::getShardingFilter(
    OperationContext* opCtx) const {
    return visit(
        OverloadedVisitor{
            [&](const CollectionPtr* collPtr) -> boost::optional<ScopedCollectionFilter> {
                auto scopedCss = CollectionShardingState::assertCollectionLockedAndAcquire(
                    opCtx, collPtr->get()->ns());
                return scopedCss->getOwnershipFilter(
                    opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup);
            },
            [](const CollectionAcquisition& acq) -> boost::optional<ScopedCollectionFilter> {
                return acq.getShardingFilter();
            }},
        _collectionPtrOrAcquisition);
}

}  // namespace mongo
