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
#include "mongo/db/repl/oplog_applier_utils.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_iterator_interface.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
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

// Used for marking intermediate oplog entries created by the resharding applier that will require
// special handling in the repl writer thread. These intermediate oplog entries serve as a message
// container and will never be written to an actual collection.
const BSONObj kReshardingOplogTag(BSON("$resharding" << 1));

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

    // TODO: handle pre/post image

    auto rawOplogBSON = oplog.toBSON();
    auto noOpOplog = uassertStatusOK(repl::MutableOplogEntry::parse(rawOplogBSON));
    noOpOplog.setObject2(rawOplogBSON);
    noOpOplog.setNss({});
    noOpOplog.setObject(BSON("$reshardingOplogApply" << 1));
    // TODO: link pre/post image
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

/**
 * Returns true if the given oplog is a special no-op oplog entry that contains the information for
 * retryable writes.
 */
bool isRetryableNoOp(const repl::OplogEntryOrGroupedInserts& oplogOrGroupedInserts) {
    if (oplogOrGroupedInserts.isGroupedInserts()) {
        return false;
    }

    const auto& op = oplogOrGroupedInserts.getOp();
    if (op.getOpType() != repl::OpTypeEnum::kNoop) {
        return false;
    }

    return op.getObject().woCompare(kReshardingOplogTag) == 0;
}

}  // anonymous namespace

ReshardingOplogApplier::ReshardingOplogApplier(
    ServiceContext* service,
    ReshardingSourceId sourceId,
    NamespaceString oplogNs,
    NamespaceString nsBeingResharded,
    UUID collUUIDBeingResharded,
    Timestamp reshardingCloneFinishedTs,
    std::unique_ptr<ReshardingDonorOplogIteratorInterface> oplogIterator,
    size_t batchSize,
    const ChunkManager& sourceChunkMgr,
    OutOfLineExecutor* executor,
    ThreadPool* writerPool)
    : _sourceId(std::move(sourceId)),
      _oplogNs(std::move(oplogNs)),
      _nsBeingResharded(std::move(nsBeingResharded)),
      _uuidBeingResharded(std::move(collUUIDBeingResharded)),
      _outputNs(_nsBeingResharded.db(),
                "system.resharding.{}"_format(_uuidBeingResharded.toString())),
      _reshardingTempNs(_nsBeingResharded.db(),
                        "{}.{}"_format(_nsBeingResharded.coll(), _oplogNs.coll())),
      _reshardingCloneFinishedTs(std::move(reshardingCloneFinishedTs)),
      _batchSize(batchSize),
      _applicationRules(ReshardingOplogApplicationRules(
          _outputNs, _reshardingTempNs, _sourceId.getShardId(), sourceChunkMgr)),
      _service(service),
      _executor(executor),
      _writerPool(writerPool),
      _oplogIter(std::move(oplogIterator)) {
    invariant(_batchSize > 0);
}

Future<void> ReshardingOplogApplier::applyUntilCloneFinishedTs() {
    invariant(_stage == ReshardingOplogApplier::Stage::kStarted);

    auto pf = makePromiseFuture<void>();
    _appliedCloneFinishTsPromise = std::move(pf.promise);

    _executor->schedule([this](Status status) {
        if (!status.isOK()) {
            _onError(status);
            return;
        }

        ThreadClient scheduleNextBatchClient(kClientName, _service);
        auto opCtx = scheduleNextBatchClient->makeOperationContext();

        try {
            uassertStatusOK(createCollection(opCtx.get(),
                                             _reshardingTempNs.db().toString(),
                                             BSON("create" << _reshardingTempNs.coll())));

            _scheduleNextBatch();
        } catch (const DBException& ex) {
            _onError(ex.toStatus());
            return;
        }
    });

    return std::move(pf.future);
}

Future<void> ReshardingOplogApplier::applyUntilDone() {
    invariant(_stage == ReshardingOplogApplier::Stage::kReachedCloningTS);

    auto pf = makePromiseFuture<void>();
    _donePromise = std::move(pf.promise);

    _executor->schedule([this](Status status) {
        if (!status.isOK()) {
            _onError(status);
            return;
        }

        try {
            _scheduleNextBatch();
        } catch (const DBException& ex) {
            _onError(ex.toStatus());
            return;
        }
    });

    return std::move(pf.future);
}

void ReshardingOplogApplier::_scheduleNextBatch() {
    if (!_oplogIter->hasMore()) {
        // It is possible that there are no more oplog entries from the last point we resumed from.
        if (_stage == ReshardingOplogApplier::Stage::kStarted) {
            _stage = ReshardingOplogApplier::Stage::kReachedCloningTS;
            _appliedCloneFinishTsPromise.emplaceValue();
            return;
        } else {
            _stage = ReshardingOplogApplier::Stage::kFinished;
            _donePromise.emplaceValue();
            return;
        }
    }

    auto scheduleBatchClient = _service->makeClient(kClientName.toString());
    AlternativeClientRegion acr(scheduleBatchClient);
    auto opCtx = cc().makeOperationContext();

    auto future = _oplogIter->getNext(opCtx.get());
    for (size_t count = 1; count < _batchSize; count++) {
        future = std::move(future).then([this](boost::optional<repl::OplogEntry> oplogEntry) {
            if (oplogEntry) {
                _preProcessAndPushOpsToBuffer(std::move(*oplogEntry));
            }

            auto batchClient = _service->makeClient(kClientName.toString());
            AlternativeClientRegion acr(batchClient);
            auto getNextOpCtx = cc().makeOperationContext();

            return _oplogIter->getNext(getNextOpCtx.get());
        });
    }

    std::move(future)
        .then([this](boost::optional<repl::OplogEntry> oplogEntry) {
            // Don't forget to push the last oplog in the batch.
            if (oplogEntry) {
                _preProcessAndPushOpsToBuffer(std::move(*oplogEntry));
            }

            auto applyBatchClient = _service->makeClient(kClientName.toString());
            AlternativeClientRegion acr(applyBatchClient);
            auto applyBatchOpCtx = cc().makeOperationContext();

            return _applyBatch(applyBatchOpCtx.get());
        })
        .then([this]() {
            if (!_currentBatchToApply.empty()) {
                auto lastApplied = _currentBatchToApply.back();

                auto scheduleBatchClient = _service->makeClient(kClientName.toString());
                AlternativeClientRegion acr(scheduleBatchClient);
                auto opCtx = cc().makeOperationContext();

                auto lastAppliedTs = _clearAppliedOpsAndStoreProgress(opCtx.get());

                if (_stage == ReshardingOplogApplier::Stage::kStarted &&
                    lastAppliedTs >= _reshardingCloneFinishedTs) {
                    _stage = ReshardingOplogApplier::Stage::kReachedCloningTS;
                    // TODO: SERVER-51741 preemptively schedule next batch
                    _appliedCloneFinishTsPromise.emplaceValue();
                    return;
                }
            }

            _executor->schedule([this](Status scheduleNextBatchStatus) {
                if (!scheduleNextBatchStatus.isOK()) {
                    _onError(scheduleNextBatchStatus);
                    return;
                }

                try {
                    _scheduleNextBatch();
                } catch (const DBException& ex) {
                    _onError(ex.toStatus());
                }
            });
        })
        .onError([this](Status status) { _onError(status); })
        .getAsync([](Status status) {
            // Do nothing.
        });
}

Future<void> ReshardingOplogApplier::_applyBatch(OperationContext* opCtx) {
    // TODO: handle config.transaction updates with derivedOps

    auto writerVectors = _fillWriterVectors(opCtx, &_currentBatchToApply, &_currentDerivedOps);
    _currentWriterVectors.swap(writerVectors);

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

repl::OplogEntry convertToNoOpWithReshardingTag(const repl::OplogEntry& oplog) {
    return repl::OplogEntry(oplog.getOpTime(),
                            oplog.getHash(),
                            repl::OpTypeEnum::kNoop,
                            oplog.getNss(),
                            boost::none /* uuid */,
                            oplog.getFromMigrate(),
                            oplog.getVersion(),
                            kReshardingOplogTag,
                            // Set the o2 field with the original oplog.
                            oplog.toBSON(),
                            oplog.getOperationSessionInfo(),
                            oplog.getUpsert(),
                            oplog.getWallClockTime(),
                            oplog.getStatementId(),
                            oplog.getPrevWriteOpTimeInTransaction(),
                            oplog.getPreImageOpTime(),
                            oplog.getPostImageOpTime(),
                            oplog.getDestinedRecipient(),
                            oplog.get_id());
}

void addDerivedOpsToWriterVector(std::vector<std::vector<const repl::OplogEntry*>>* writerVectors,
                                 const std::vector<repl::OplogEntry>& derivedOps) {
    for (auto&& op : derivedOps) {
        invariant(op.getObject().woCompare(kReshardingOplogTag) == 0);
        uassert(4990403,
                "expected resharding derived oplog to have session id: {}"_format(
                    op.toBSON().toString()),
                op.getSessionId());

        LogicalSessionIdHash hasher;
        auto writerId = hasher(*op.getSessionId()) % writerVectors->size();
        (*writerVectors)[writerId].push_back(&op);
    }
}

std::vector<std::vector<const repl::OplogEntry*>> ReshardingOplogApplier::_fillWriterVectors(
    OperationContext* opCtx, OplogBatch* batch, OplogBatch* derivedOps) {
    std::vector<std::vector<const repl::OplogEntry*>> writerVectors(
        _writerPool->getStats().numThreads);
    repl::CachedCollectionProperties collPropertiesCache;

    LogicalSessionIdMap<RetryableOpsList> sessionTracker;

    for (auto&& op : *batch) {
        uassert(5012000,
                "Resharding oplog application does not support prepared transactions.",
                !op.shouldPrepare());
        uassert(5012001,
                "Resharding oplog application does not support prepared transactions.",
                !op.isPreparedCommit());

        if (op.getOpType() == repl::OpTypeEnum::kNoop)
            continue;

        repl::OplogApplierUtils::addToWriterVector(
            opCtx, &op, &writerVectors, &collPropertiesCache);

        if (auto sessionId = op.getSessionId()) {
            auto& retryableOpList = sessionTracker[*sessionId];
            auto txnNumber = *op.getTxnNumber();

            if (retryableOpList.txnNum == txnNumber) {
                retryableOpList.ops.push_back(&op);
            } else if (retryableOpList.txnNum < txnNumber) {
                retryableOpList.ops.clear();
                retryableOpList.ops.push_back(&op);
                retryableOpList.txnNum = txnNumber;
            } else {
                uasserted(4990401,
                          str::stream() << "retryable oplog applier for " << _sourceId.toBSON()
                                        << " encountered out of order txnNum, saw " << op.toBSON()
                                        << " after " << retryableOpList.ops.front()->toBSON());
            }
        }
    }

    for (const auto& sessionsToUpdate : sessionTracker) {
        for (const auto& op : sessionsToUpdate.second.ops) {
            auto noOpWithPrePost = convertToNoOpWithReshardingTag(*op);
            derivedOps->push_back(std::move(noOpWithPrePost));
        }
    }

    addDerivedOpsToWriterVector(&writerVectors, _currentDerivedOps);

    return writerVectors;
}

Status ReshardingOplogApplier::_applyOplogBatchPerWorker(
    std::vector<const repl::OplogEntry*>* ops) {
    auto opCtx = cc().makeOperationContext();

    return repl::OplogApplierUtils::applyOplogBatchCommon(
        opCtx.get(),
        ops,
        repl::OplogApplication::Mode::kInitialSync,
        false /* allowNamespaceNotFoundErrorsOnCrudOps */,
        [this](OperationContext* opCtx,
               const repl::OplogEntryOrGroupedInserts& opOrInserts,
               repl::OplogApplication::Mode mode) {
            return _applyOplogEntryOrGroupedInserts(opCtx, opOrInserts, mode);
        });
}

Status ReshardingOplogApplier::_applyOplogEntryOrGroupedInserts(
    OperationContext* opCtx,
    const repl::OplogEntryOrGroupedInserts& entryOrGroupedInserts,
    repl::OplogApplication::Mode oplogApplicationMode) {
    // Unlike normal secondary replication, we want the write to generate it's own oplog entry.
    invariant(opCtx->writesAreReplicated());

    // Ensure context matches that of _applyOplogBatchPerWorker.
    invariant(oplogApplicationMode == repl::OplogApplication::Mode::kInitialSync);

    auto op = entryOrGroupedInserts.getOp();

    // We don't care about applied stats in resharding.
    auto incrementOpsAppliedStats = [] {};

    if (isRetryableNoOp(entryOrGroupedInserts)) {
        return insertOplogAndUpdateConfigForRetryable(opCtx, entryOrGroupedInserts.getOp());
    }

    invariant(DocumentValidationSettings::get(opCtx).isSchemaValidationDisabled());

    auto opType = op.getOpType();
    if (opType == repl::OpTypeEnum::kNoop) {
        return Status::OK();
    } else if (resharding::gUseReshardingOplogApplicationRules) {
        if (opType == repl::OpTypeEnum::kInsert) {
            return _applicationRules.applyOperation(opCtx, entryOrGroupedInserts);
        } else {
            // TODO SERVER-49902 call ReshardingOplogApplicationRules::applyOperation for deletes
            // TODO SERVER-49903 call ReshardingOplogApplicationRules::applyOperation for updates
            return repl::OplogApplierUtils::applyOplogEntryOrGroupedInsertsCommon(
                opCtx,
                entryOrGroupedInserts,
                oplogApplicationMode,
                incrementOpsAppliedStats,
                nullptr);
        }
    } else {
        // We always use oplog application mode 'kInitialSync', because we're applying oplog entries
        // to a cloned database the way initial sync does.
        return repl::OplogApplierUtils::applyOplogEntryOrGroupedInsertsCommon(
            opCtx, entryOrGroupedInserts, oplogApplicationMode, incrementOpsAppliedStats, nullptr);
    }

    MONGO_UNREACHABLE;
}

// TODO: use MutableOplogEntry to handle prePostImageOps? Because OplogEntry tries to create BSON
// and can cause size too big.
void ReshardingOplogApplier::_preProcessAndPushOpsToBuffer(repl::OplogEntry oplog) {
    uassert(5012002,
            str::stream() << "trying to apply oplog not belonging to ns " << _nsBeingResharded
                          << " during resharding: " << oplog.toBSON(),
            _nsBeingResharded == oplog.getNss());
    uassert(5012005,
            str::stream() << "trying to apply oplog with a different UUID from "
                          << _uuidBeingResharded << " during resharding: " << oplog.toBSON(),
            _uuidBeingResharded == oplog.getUuid());

    auto newOplog = repl::OplogEntry(oplog.getOpTime(),
                                     oplog.getHash(),
                                     oplog.getOpType(),
                                     _outputNs,
                                     boost::none /* uuid */,
                                     oplog.getFromMigrate(),
                                     oplog.getVersion(),
                                     oplog.getObject(),
                                     oplog.getObject2(),
                                     oplog.getOperationSessionInfo(),
                                     oplog.getUpsert(),
                                     oplog.getWallClockTime(),
                                     oplog.getStatementId(),
                                     oplog.getPrevWriteOpTimeInTransaction(),
                                     oplog.getPreImageOpTime(),
                                     oplog.getPostImageOpTime(),
                                     oplog.getDestinedRecipient(),
                                     oplog.get_id());

    _currentBatchToApply.push_back(std::move(newOplog));
}

void ReshardingOplogApplier::_onError(Status status) {
    if (_stage == ReshardingOplogApplier::Stage::kStarted) {
        _appliedCloneFinishTsPromise.setError(status);
    } else {
        _donePromise.setError(status);
    }

    _stage = ReshardingOplogApplier::Stage::kErrorOccurred;
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

}  // namespace mongo
