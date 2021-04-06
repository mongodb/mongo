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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_oplog_applier.h"

#include <fmt/format.h>

#include "mongo/base/simple_string_data_comparator.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/oplog_applier_utils.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/uuid.h"

namespace mongo {

using namespace fmt::literals;

namespace {

ServiceContext::UniqueClient makeKillableClient(ServiceContext* serviceContext, StringData name) {
    auto client = serviceContext->makeClient(name.toString());
    stdx::lock_guard<Client> lk(*client);
    client->setSystemOperationKillableByStepdown(lk);
    return client;
}

}  // anonymous namespace

ReshardingOplogApplier::ReshardingOplogApplier(
    std::unique_ptr<Env> env,
    ReshardingSourceId sourceId,
    NamespaceString oplogNs,
    NamespaceString nsBeingResharded,
    UUID collUUIDBeingResharded,
    std::vector<NamespaceString> allStashNss,
    size_t myStashIdx,
    Timestamp reshardingCloneFinishedTs,
    std::unique_ptr<ReshardingDonorOplogIteratorInterface> oplogIterator,
    const ChunkManager& sourceChunkMgr)
    : _env(std::move(env)),
      _sourceId(std::move(sourceId)),
      _oplogNs(std::move(oplogNs)),
      _nsBeingResharded(std::move(nsBeingResharded)),
      _uuidBeingResharded(std::move(collUUIDBeingResharded)),
      _outputNs(constructTemporaryReshardingNss(_nsBeingResharded.db(), _uuidBeingResharded)),
      _reshardingCloneFinishedTs(std::move(reshardingCloneFinishedTs)),
      _batchPreparer{CollatorInterface::cloneCollator(sourceChunkMgr.getDefaultCollator())},
      _crudApplication{
          _outputNs, std::move(allStashNss), myStashIdx, _sourceId.getShardId(), sourceChunkMgr},
      _sessionApplication{},
      _batchApplier{_crudApplication, _sessionApplication},
      _oplogIter(std::move(oplogIterator)) {}

ExecutorFuture<void> ReshardingOplogApplier::applyUntilCloneFinishedTs(
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory factory) {
    invariant(_stage == ReshardingOplogApplier::Stage::kStarted);

    return ExecutorFuture(executor)
        .then([this, executor, cancelToken, factory] {
            return _scheduleNextBatch(executor, cancelToken, factory);
        })
        .onError([this](Status status) { return _onError(status); })
        // There isn't a guarantee that the reference count to `executor` has been decremented after
        // .then() returns. We schedule a trivial task on the task executor to ensure the callback's
        // destructor has run. Otherwise `executor` could end up outliving the ServiceContext and
        // triggering an invariant due to the task executor's thread having a Client still.
        .onCompletion([](auto x) { return x; });
}

ExecutorFuture<void> ReshardingOplogApplier::applyUntilDone(
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory factory) {
    invariant(_stage == ReshardingOplogApplier::Stage::kReachedCloningTS);

    return ExecutorFuture(executor)
        .then([this, executor, cancelToken, factory] {
            return _scheduleNextBatch(executor, cancelToken, factory);
        })
        .onError([this](Status status) { return _onError(status); })
        // There isn't a guarantee that the reference count to `executor` has been decremented after
        // .then() returns. We schedule a trivial task on the task executor to ensure the callback's
        // destructor has run. Otherwise `executor` could end up outliving the ServiceContext and
        // triggering an invariant due to the task executor's thread having a Client still.
        .onCompletion([](auto x) { return x; });
}

ExecutorFuture<void> ReshardingOplogApplier::_scheduleNextBatch(
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory factory) {
    return ExecutorFuture(executor)
        .then([this, executor, cancelToken, factory] {
            auto batchClient = makeKillableClient(_service(), kClientName);
            AlternativeClientRegion acr(batchClient);

            return _oplogIter->getNextBatch(executor, cancelToken, factory);
        })
        .then([this, executor, cancelToken, factory](OplogBatch batch) {
            LOGV2_DEBUG(5391002, 3, "Starting batch", "batchSize"_attr = batch.size());
            _currentBatchToApply = std::move(batch);

            return _applyBatch(executor, cancelToken, factory, false /* isForSessionApplication */);
        })
        .then([this, executor, cancelToken, factory] {
            return _applyBatch(executor, cancelToken, factory, true /* isForSessionApplication */);
        })
        .then([this, factory] {
            if (_currentBatchToApply.empty()) {
                // It is possible that there are no more oplog entries from the last point we
                // resumed from.
                if (_stage == ReshardingOplogApplier::Stage::kStarted) {
                    _stage = ReshardingOplogApplier::Stage::kReachedCloningTS;
                } else if (_stage == ReshardingOplogApplier::Stage::kReachedCloningTS) {
                    _stage = ReshardingOplogApplier::Stage::kFinished;
                }
                return false;
            }

            auto lastApplied = _currentBatchToApply.back();

            auto scheduleBatchClient = makeKillableClient(_service(), kClientName);
            AlternativeClientRegion acr(scheduleBatchClient);
            auto opCtx = factory.makeOperationContext(&cc());

            auto lastAppliedTs = _clearAppliedOpsAndStoreProgress(opCtx.get());

            if (_stage == ReshardingOplogApplier::Stage::kStarted &&
                lastAppliedTs >= _reshardingCloneFinishedTs) {
                _stage = ReshardingOplogApplier::Stage::kReachedCloningTS;
                // TODO: SERVER-51741 preemptively schedule next batch
                return false;
            }

            return true;
        })
        .then([this, executor, cancelToken, factory](bool moreToApply) {
            if (!moreToApply) {
                return ExecutorFuture(executor);
            }

            if (cancelToken.isCanceled()) {
                return ExecutorFuture<void>(
                    executor,
                    Status{ErrorCodes::CallbackCanceled,
                           "Resharding oplog applier aborting due to abort or stepdown"});
            }
            return _scheduleNextBatch(executor, cancelToken, factory);
        });
}

SemiFuture<void> ReshardingOplogApplier::_applyBatch(
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory factory,
    bool isForSessionApplication) {
    auto currentWriterVectors = [&] {
        if (isForSessionApplication) {
            return _batchPreparer.makeSessionOpWriterVectors(_currentBatchToApply);
        } else {
            return _batchPreparer.makeCrudOpWriterVectors(_currentBatchToApply, _currentDerivedOps);
        }
    }();

    CancellationSource errorSource(cancelToken);

    std::vector<SharedSemiFuture<void>> batchApplierFutures;
    batchApplierFutures.reserve(currentWriterVectors.size());

    for (auto&& writer : currentWriterVectors) {
        if (!writer.empty()) {
            batchApplierFutures.emplace_back(
                _batchApplier.applyBatch(std::move(writer), executor, errorSource.token(), factory)
                    .share());
        }
    }

    return resharding::cancelWhenAnyErrorThenQuiesce(batchApplierFutures, executor, errorSource)
        .onError([](Status status) {
            LOGV2_ERROR(
                5012004, "Failed to apply operation in resharding", "error"_attr = redact(status));
            return status;
        })
        .semi();
}

Status ReshardingOplogApplier::_onError(Status status) {
    _stage = ReshardingOplogApplier::Stage::kErrorOccurred;
    return status;
}

boost::optional<ReshardingOplogApplierProgress> ReshardingOplogApplier::checkStoredProgress(
    OperationContext* opCtx, const ReshardingSourceId& id) {
    DBDirectClient client(opCtx);
    auto doc = client.findOne(
        NamespaceString::kReshardingApplierProgressNamespace.ns(),
        BSON(ReshardingOplogApplierProgress::kOplogSourceIdFieldName << id.toBSON()));

    if (doc.isEmpty()) {
        return boost::none;
    }

    IDLParserErrorContext ctx("ReshardingOplogApplierProgress");
    return ReshardingOplogApplierProgress::parse(ctx, doc);
}

Timestamp ReshardingOplogApplier::_clearAppliedOpsAndStoreProgress(OperationContext* opCtx) {
    const auto& lastOplog = _currentBatchToApply.back();

    auto oplogId =
        ReshardingDonorOplogId::parse(IDLParserErrorContext("ReshardingOplogApplierStoreProgress"),
                                      lastOplog.get_id()->getDocument().toBson());

    // TODO: take multi statement transactions into account.

    auto lastAppliedTs = lastOplog.getTimestamp();

    PersistentTaskStore<ReshardingOplogApplierProgress> store(
        NamespaceString::kReshardingApplierProgressNamespace);

    BSONObjBuilder builder;
    builder.append("$set",
                   BSON(ReshardingOplogApplierProgress::kProgressFieldName << oplogId.toBSON()));
    builder.append("$inc",
                   BSON(ReshardingOplogApplierProgress::kNumEntriesAppliedFieldName
                        << static_cast<long long>(_currentBatchToApply.size())));

    store.upsert(
        opCtx,
        QUERY(ReshardingOplogApplierProgress::kOplogSourceIdFieldName << _sourceId.toBSON()),
        builder.obj());
    _env->metrics()->onOplogEntriesApplied(_currentBatchToApply.size());

    _currentBatchToApply.clear();
    _currentDerivedOps.clear();

    return lastAppliedTs;
}

NamespaceString ReshardingOplogApplier::ensureStashCollectionExists(
    OperationContext* opCtx,
    const UUID& existingUUID,
    const ShardId& donorShardId,
    const CollectionOptions& options) {
    auto nss = getLocalConflictStashNamespace(existingUUID, donorShardId);

    resharding::data_copy::ensureCollectionExists(opCtx, nss, options);
    return nss;
}

}  // namespace mongo
