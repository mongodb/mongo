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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

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

/**
 * Insert a no-op oplog entry that contains the pre/post image document from a retryable write.
 */
repl::OpTime insertPrePostImageOplogEntry(OperationContext* opCtx,
                                          const repl::DurableOplogEntry& prePostImageOp) {
    uassert(4990408,
            str::stream() << "expected a no-op oplog for pre/post image oplog: "
                          << redact(prePostImageOp.toBSON()),
            prePostImageOp.getOpType() == repl::OpTypeEnum::kNoop);

    auto noOpOplog = uassertStatusOK(repl::MutableOplogEntry::parse(prePostImageOp.toBSON()));
    // Reset OpTime so logOp() can assign a new one.
    noOpOplog.setOpTime(OplogSlot());
    noOpOplog.setWallClockTime(Date_t::now());

    return writeConflictRetry(
        opCtx,
        "InsertPrePostImageOplogEntryForResharding",
        NamespaceString::kSessionTransactionsTableNamespace.ns(),
        [&] {
            // Need to take global lock here so repl::logOp will not unlock it and trigger the
            // invariant that disallows unlocking global lock while inside a WUOW. Take the
            // transaction table db lock to ensure the same lock ordering with normal replicated
            // updates to the table.
            Lock::DBLock lk(
                opCtx, NamespaceString::kSessionTransactionsTableNamespace.db(), MODE_IX);
            WriteUnitOfWork wunit(opCtx);

            const auto& oplogOpTime = repl::logOp(opCtx, &noOpOplog);

            uassert(4990409,
                    str::stream() << "Failed to create new oplog entry for oplog with opTime: "
                                  << noOpOplog.getOpTime().toString() << ": "
                                  << redact(noOpOplog.toBSON()),
                    !oplogOpTime.isNull());

            wunit.commit();

            return oplogOpTime;
        });
}

/**
 * Writes the oplog entries and updates to config.transactions for enabling retrying the write
 * described in the oplog entry.
 */
Status insertOplogAndUpdateConfigForRetryable(OperationContext* opCtx,
                                              const repl::OplogEntry& oplog) {
    auto txnNumber = *oplog.getTxnNumber();

    opCtx->setLogicalSessionId(*oplog.getSessionId());
    opCtx->setTxnNumber(txnNumber);

    boost::optional<MongoDOperationContextSession> scopedSession;
    scopedSession.emplace(opCtx);

    auto txnParticipant = TransactionParticipant::get(opCtx);
    uassert(4990400, "Failed to get transaction Participant", txnParticipant);
    const auto stmtId = *oplog.getStatementId();

    try {
        txnParticipant.beginOrContinue(opCtx, txnNumber, boost::none, boost::none);

        if (txnParticipant.checkStatementExecuted(opCtx, stmtId)) {
            // Skip the incoming statement because it has already been logged locally.
            return Status::OK();
        }
    } catch (const DBException& ex) {
        if (ex.code() == ErrorCodes::TransactionTooOld) {
            return Status::OK();
        } else if (ex.code() == ErrorCodes::IncompleteTransactionHistory) {
            // If the transaction chain is incomplete because oplog was truncated, just ignore the
            // incoming oplog and don't attempt to 'patch up' the missing pieces.
            // This can also occur when txnNum == activeTxnNum. This can only happen when (lsid,
            // txnNum) pair is reused. We are not going to update config.transactions and let the
            // retry error out on this shard for this case.
            return Status::OK();
        } else if (ex.code() == ErrorCodes::PreparedTransactionInProgress) {
            // TODO SERVER-53139 Change to not block here.
            auto txnFinishes = txnParticipant.onExitPrepare();
            scopedSession.reset();
            txnFinishes.wait();
            return insertOplogAndUpdateConfigForRetryable(opCtx, oplog);
        }

        throw;
    }

    repl::OpTime prePostImageOpTime;
    if (auto preImageOp = oplog.getPreImageOp()) {
        prePostImageOpTime = insertPrePostImageOplogEntry(opCtx, *preImageOp);
    } else if (auto postImageOp = oplog.getPostImageOp()) {
        prePostImageOpTime = insertPrePostImageOplogEntry(opCtx, *postImageOp);
    }

    auto rawOplogBSON = oplog.getEntry().toBSON();
    auto noOpOplog = uassertStatusOK(repl::MutableOplogEntry::parse(rawOplogBSON));
    noOpOplog.setObject2(rawOplogBSON);
    noOpOplog.setNss({});
    noOpOplog.setObject(BSON("$reshardingOplogApply" << 1));

    if (oplog.getPreImageOp()) {
        noOpOplog.setPreImageOpTime(prePostImageOpTime);
    } else if (oplog.getPostImageOp()) {
        noOpOplog.setPostImageOpTime(prePostImageOpTime);
    }

    noOpOplog.setPrevWriteOpTimeInTransaction(txnParticipant.getLastWriteOpTime());
    noOpOplog.setOpType(repl::OpTypeEnum::kNoop);
    // Reset OpTime so logOp() can assign a new one.
    noOpOplog.setOpTime(OplogSlot());
    noOpOplog.setWallClockTime(Date_t::now());

    writeConflictRetry(
        opCtx,
        "ReshardingUpdateConfigTransaction",
        NamespaceString::kSessionTransactionsTableNamespace.ns(),
        [&] {
            // Need to take global lock here so repl::logOp will not unlock it and trigger the
            // invariant that disallows unlocking global lock while inside a WUOW. Take the
            // transaction table db lock to ensure the same lock ordering with normal replicated
            // updates to the table.
            Lock::DBLock lk(
                opCtx, NamespaceString::kSessionTransactionsTableNamespace.db(), MODE_IX);
            WriteUnitOfWork wunit(opCtx);

            const auto& oplogOpTime = repl::logOp(opCtx, &noOpOplog);

            uassert(4990402,
                    str::stream() << "Failed to create new oplog entry for oplog with opTime: "
                                  << noOpOplog.getOpTime().toString() << ": "
                                  << redact(noOpOplog.toBSON()),
                    !oplogOpTime.isNull());

            SessionTxnRecord sessionTxnRecord;
            sessionTxnRecord.setSessionId(*oplog.getSessionId());
            sessionTxnRecord.setTxnNum(txnNumber);
            sessionTxnRecord.setLastWriteOpTime(oplogOpTime);
            sessionTxnRecord.setLastWriteDate(noOpOplog.getWallClockTime());
            txnParticipant.onRetryableWriteCloningCompleted(opCtx, {stmtId}, sessionTxnRecord);

            wunit.commit();
        });

    return Status::OK();
}

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
    ServiceContext* service,
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
    : _sourceId(std::move(sourceId)),
      _oplogNs(std::move(oplogNs)),
      _nsBeingResharded(std::move(nsBeingResharded)),
      _uuidBeingResharded(std::move(collUUIDBeingResharded)),
      _outputNs(_nsBeingResharded.db(),
                "system.resharding.{}"_format(_uuidBeingResharded.toString())),
      _reshardingCloneFinishedTs(std::move(reshardingCloneFinishedTs)),
      _batchPreparer{CollatorInterface::cloneCollator(sourceChunkMgr.getDefaultCollator())},
      _applicationRules(ReshardingOplogApplicationRules(
          _outputNs, std::move(allStashNss), myStashIdx, _sourceId.getShardId(), sourceChunkMgr)),
      _service(service),
      _executor(std::move(executor)),
      _writerPool(writerPool),
      _oplogIter(std::move(oplogIterator)) {}

ExecutorFuture<void> ReshardingOplogApplier::applyUntilCloneFinishedTs() {
    invariant(_stage == ReshardingOplogApplier::Stage::kStarted);

    // It is safe to capture `this` because PrimaryOnlyService and RecipientStateMachine
    // collectively guarantee that the ReshardingOplogApplier instances will outlive `_executor` and
    // `_writerPool`.
    return ExecutorFuture(_executor)
        .then([this] { return _scheduleNextBatch(); })
        .onError([this](Status status) { return _onError(status); });
}

ExecutorFuture<void> ReshardingOplogApplier::applyUntilDone() {
    invariant(_stage == ReshardingOplogApplier::Stage::kReachedCloningTS);

    // It is safe to capture `this` because PrimaryOnlyService and RecipientStateMachine
    // collectively guarantee that the ReshardingOplogApplier instances will outlive `_executor` and
    // `_writerPool`.
    return ExecutorFuture(_executor)
        .then([this] { return _scheduleNextBatch(); })
        .onError([this](Status status) { return _onError(status); });
}

ExecutorFuture<void> ReshardingOplogApplier::_scheduleNextBatch() {
    return ExecutorFuture(_executor)
        .then([this] {
            auto batchClient = makeKillableClient(_service, kClientName);
            AlternativeClientRegion acr(batchClient);

            return _oplogIter->getNextBatch(_executor);
        })
        .then([this](OplogBatch batch) {
            _currentBatchToApply = std::move(batch);

            auto applyBatchClient = makeKillableClient(_service, kClientName);
            AlternativeClientRegion acr(applyBatchClient);
            auto applyBatchOpCtx = makeInterruptibleOperationContext();

            return _applyBatch(applyBatchOpCtx.get(), false /* isForSessionApplication */);
        })
        .then([this] {
            auto applyBatchClient = makeKillableClient(_service, kClientName);
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

            auto scheduleBatchClient = makeKillableClient(_service, kClientName);
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
        .then([this](bool moreToApply) {
            if (!moreToApply) {
                return ExecutorFuture(_executor);
            }
            return _scheduleNextBatch();
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

    return repl::OplogApplierUtils::applyOplogBatchCommon(
        opCtx.get(),
        ops,
        repl::OplogApplication::Mode::kInitialSync,
        false /* allowNamespaceNotFoundErrorsOnCrudOps */,
        [this](OperationContext* opCtx,
               const repl::OplogEntryOrGroupedInserts& opOrInserts,
               repl::OplogApplication::Mode mode) {
            invariant(mode == repl::OplogApplication::Mode::kInitialSync);
            return _applyOplogEntryOrGroupedInserts(opCtx, opOrInserts);
        });
}

Status ReshardingOplogApplier::_applyOplogEntryOrGroupedInserts(
    OperationContext* opCtx, const repl::OplogEntryOrGroupedInserts& entryOrGroupedInserts) {
    // Unlike normal secondary replication, we want the write to generate it's own oplog entry.
    invariant(opCtx->writesAreReplicated());

    auto op = entryOrGroupedInserts.getOp();

    if (op.isForReshardingSessionApplication()) {
        return insertOplogAndUpdateConfigForRetryable(opCtx, entryOrGroupedInserts.getOp());
    }

    invariant(DocumentValidationSettings::get(opCtx).isSchemaValidationDisabled());

    invariant(op.isCrudOpType());
    return _applicationRules.applyOperation(opCtx, entryOrGroupedInserts);
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
    store.upsert(
        opCtx,
        QUERY(ReshardingOplogApplierProgress::kOplogSourceIdFieldName << _sourceId.toBSON()),
        BSON("$set" << BSON(ReshardingOplogApplierProgress::kProgressFieldName
                            << oplogId.toBSON())));

    _currentBatchToApply.clear();
    _currentDerivedOps.clear();

    return lastAppliedTs;
}

NamespaceString ReshardingOplogApplier::ensureStashCollectionExists(OperationContext* opCtx,
                                                                    const UUID& existingUUID,
                                                                    const ShardId& donorShardId) {
    auto nss = NamespaceString{NamespaceString::kConfigDb,
                               "localReshardingConflictStash.{}.{}"_format(
                                   existingUUID.toString(), donorShardId.toString())};

    resharding::data_copy::ensureCollectionExists(opCtx, nss);
    return nss;
}

}  // namespace mongo
