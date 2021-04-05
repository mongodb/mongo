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

ServiceContext::UniqueOperationContext makeInterruptibleOperationContext() {
    auto opCtx = cc().makeOperationContext();
    opCtx->setAlwaysInterruptAtStepDownOrUp();
    return opCtx;
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
    const ChunkManager& sourceChunkMgr,
    std::shared_ptr<executor::TaskExecutor> executor,
    ThreadPool* writerPool)
    : _env(std::move(env)),
      _sourceId(std::move(sourceId)),
      _oplogNs(std::move(oplogNs)),
      _nsBeingResharded(std::move(nsBeingResharded)),
      _uuidBeingResharded(std::move(collUUIDBeingResharded)),
      _outputNs(constructTemporaryReshardingNss(_nsBeingResharded.db(), _uuidBeingResharded)),
      _reshardingCloneFinishedTs(std::move(reshardingCloneFinishedTs)),
      _batchPreparer{CollatorInterface::cloneCollator(sourceChunkMgr.getDefaultCollator())},
      _sessionApplication{},
      _applicationRules(ReshardingOplogApplicationRules(
          _outputNs, std::move(allStashNss), myStashIdx, _sourceId.getShardId(), sourceChunkMgr)),
      _executor(std::move(executor)),
      _writerPool(writerPool),
      _oplogIter(std::move(oplogIterator)) {}

ExecutorFuture<void> ReshardingOplogApplier::applyUntilCloneFinishedTs(
    CancellationToken cancelToken) {
    invariant(_stage == ReshardingOplogApplier::Stage::kStarted);

    // It is safe to capture `this` because PrimaryOnlyService and RecipientStateMachine
    // collectively guarantee that the ReshardingOplogApplier instances will outlive `_executor` and
    // `_writerPool`.
    return ExecutorFuture(_executor)
        .then([this, cancelToken] { return _scheduleNextBatch(cancelToken); })
        .onError([this](Status status) { return _onError(status); });
}

ExecutorFuture<void> ReshardingOplogApplier::applyUntilDone(CancellationToken cancelToken) {
    invariant(_stage == ReshardingOplogApplier::Stage::kReachedCloningTS);

    // It is safe to capture `this` because PrimaryOnlyService and RecipientStateMachine
    // collectively guarantee that the ReshardingOplogApplier instances will outlive `_executor` and
    // `_writerPool`.
    return ExecutorFuture(_executor)
        .then([this, cancelToken] { return _scheduleNextBatch(cancelToken); })
        .onError([this](Status status) { return _onError(status); });
}

ExecutorFuture<void> ReshardingOplogApplier::_scheduleNextBatch(CancellationToken cancelToken) {
    return ExecutorFuture(_executor)
        .then([this, cancelToken] {
            auto batchClient = makeKillableClient(_service(), kClientName);
            AlternativeClientRegion acr(batchClient);

            return _oplogIter->getNextBatch(_executor, cancelToken);
        })
        .then([this](OplogBatch batch) {
            LOGV2_DEBUG(5391002, 3, "Starting batch", "batchSize"_attr = batch.size());
            _currentBatchToApply = std::move(batch);

            auto applyBatchClient = makeKillableClient(_service(), kClientName);
            AlternativeClientRegion acr(applyBatchClient);
            auto applyBatchOpCtx = makeInterruptibleOperationContext();

            return _applyBatch(applyBatchOpCtx.get(), false /* isForSessionApplication */);
        })
        .then([this] {
            auto applyBatchClient = makeKillableClient(_service(), kClientName);
            AlternativeClientRegion acr(applyBatchClient);
            auto applyBatchOpCtx = makeInterruptibleOperationContext();

            return _applyBatch(applyBatchOpCtx.get(), true /* isForSessionApplication */);
        })
        .then([this] {
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
            auto opCtx = makeInterruptibleOperationContext();

            auto lastAppliedTs = _clearAppliedOpsAndStoreProgress(opCtx.get());

            if (_stage == ReshardingOplogApplier::Stage::kStarted &&
                lastAppliedTs >= _reshardingCloneFinishedTs) {
                _stage = ReshardingOplogApplier::Stage::kReachedCloningTS;
                // TODO: SERVER-51741 preemptively schedule next batch
                return false;
            }

            return true;
        })
        .then([this, cancelToken](bool moreToApply) {
            if (!moreToApply) {
                return ExecutorFuture(_executor);
            }

            if (cancelToken.isCanceled()) {
                return ExecutorFuture<void>(
                    _executor,
                    Status{ErrorCodes::CallbackCanceled,
                           "Resharding oplog applier aborting due to abort or stepdown"});
            }
            return _scheduleNextBatch(cancelToken);
        });
}

Future<void> ReshardingOplogApplier::_applyBatch(OperationContext* opCtx,
                                                 bool isForSessionApplication) {
    if (isForSessionApplication) {
        _currentWriterVectors = _batchPreparer.makeSessionOpWriterVectors(_currentBatchToApply);
    } else {
        _currentWriterVectors =
            _batchPreparer.makeCrudOpWriterVectors(_currentBatchToApply, _currentDerivedOps);
    }

    auto pf = makePromiseFuture<void>();

    {
        stdx::lock_guard lock(_mutex);
        _currentApplyBatchPromise = std::move(pf.promise);
        _remainingWritersToWait = _currentWriterVectors.size();
        _currentBatchConsolidatedStatus = Status::OK();
    }

    for (auto&& writer : _currentWriterVectors) {
        if (writer.empty()) {
            _onWriterVectorDone(Status::OK());
            continue;
        }

        _writerPool->schedule([this, &writer](auto scheduleStatus) {
            if (!scheduleStatus.isOK()) {
                _onWriterVectorDone(scheduleStatus);
            } else {
                _onWriterVectorDone(_applyOplogBatchPerWorker(&writer));
            }
        });
    }

    return std::move(pf.future);
}

Status ReshardingOplogApplier::_applyOplogBatchPerWorker(
    std::vector<const repl::OplogEntry*>* ops) {
    auto opCtx = makeInterruptibleOperationContext();

    for (const auto& op : *ops) {
        try {
            auto status = _applyOplogEntry(opCtx.get(), *op);

            if (!status.isOK()) {
                return status;
            }
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }

    return Status::OK();
}

Status ReshardingOplogApplier::_applyOplogEntry(OperationContext* opCtx,
                                                const repl::OplogEntry& op) {
    // Unlike normal secondary replication, we want the write to generate it's own oplog entry.
    invariant(opCtx->writesAreReplicated());

    if (op.isForReshardingSessionApplication()) {
        auto hitPreparedTxn = _sessionApplication.tryApplyOperation(opCtx, op);

        if (hitPreparedTxn) {
            hitPreparedTxn->get(opCtx);
            uassert(5538400,
                    str::stream() << "Hit prepared transaction twice while applying oplog entry: "
                                  << redact(op.toBSONForLogging()),
                    !_sessionApplication.tryApplyOperation(opCtx, op));
        }

        return Status::OK();
    }

    invariant(op.isCrudOpType());
    return _applicationRules.applyOperation(opCtx, op);
}

Status ReshardingOplogApplier::_onError(Status status) {
    _stage = ReshardingOplogApplier::Stage::kErrorOccurred;
    return status;
}

void ReshardingOplogApplier::_onWriterVectorDone(Status status) {
    auto finalStatus = ([this, &status] {
        boost::optional<Status> statusToReturn;

        stdx::lock_guard lock(_mutex);
        invariant(_remainingWritersToWait > 0);
        _remainingWritersToWait--;

        if (!status.isOK()) {
            LOGV2_ERROR(
                5012004, "Failed to apply operation in resharding", "error"_attr = redact(status));
            _currentBatchConsolidatedStatus = std::move(status);
        }

        if (_remainingWritersToWait == 0) {
            statusToReturn = _currentBatchConsolidatedStatus;
        }

        return statusToReturn;
    })();

    // Note: We ready _currentApplyBatchPromise without holding the mutex so the
    // ReshardingOplogApplier is safe to destruct immediately after a batch has been applied.
    if (finalStatus) {
        if (finalStatus->isOK()) {
            _currentApplyBatchPromise.emplaceValue();
        } else {
            _currentApplyBatchPromise.setError(*finalStatus);
        }
    }
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
