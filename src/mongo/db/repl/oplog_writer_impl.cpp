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

#include "mongo/db/repl/oplog_writer_impl.h"

#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/repl/initial_syncer.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/db/storage/storage_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {

namespace {

const auto changeCollNss = NamespaceString::makeChangeCollectionNSS(boost::none);

auto checkFeatureFlagReduceMajorityWriteLatencyFn = [] {
    return feature_flags::gReduceMajorityWriteLatency.isEnabled(
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
};

auto& oplogWriterMetric = *MetricBuilder<OplogWriterStats>{"repl.write"}.setPredicate(
    checkFeatureFlagReduceMajorityWriteLatencyFn);

Status insertDocsToOplogAndChangeCollections(OperationContext* opCtx,
                                             std::vector<InsertStatement>::const_iterator begin,
                                             std::vector<InsertStatement>::const_iterator end,
                                             bool writeOplogColl,
                                             bool writeChangeColl,
                                             OplogWriter::Observer* observer) {
    WriteUnitOfWork wuow(opCtx);
    boost::optional<AutoGetOplog> autoOplog;
    boost::optional<ChangeStreamChangeCollectionManager::ChangeCollectionsWriter> ccw;

    // Acquire locks. We must acquire the locks for all collections we intend to write to before
    // performing any writes. This avoids potential deadlocks created by waiting for locks while
    // having generated oplog holes.
    if (writeOplogColl) {
        autoOplog.emplace(opCtx, OplogAccessMode::kWrite);
    }
    if (writeChangeColl) {
        ccw.emplace(ChangeStreamChangeCollectionManager::get(opCtx).createChangeCollectionsWriter(
            opCtx, begin, end, nullptr /* opDebug */));
        ccw->acquireLocks();
    }

    // Write the oplog entries to the oplog collection.
    if (writeOplogColl) {
        auto& oplogColl = autoOplog->getCollection();
        if (!oplogColl) {
            return {ErrorCodes::NamespaceNotFound, "Oplog collection does not exist"};
        }
        auto status = collection_internal::insertDocuments(
            opCtx, oplogColl, begin, end, nullptr /* OpDebug */, false /* fromMigrate */);
        if (!status.isOK()) {
            return status;
        }
        observer->onWriteOplogCollection(begin, end);
    }

    // Write the oplog entries to the tenant respective change collections.
    if (writeChangeColl) {
        auto status = ccw->write();
        if (!status.isOK()) {
            return status;
        }
        observer->onWriteChangeCollections(begin, end);
    }

    wuow.commit();

    return Status::OK();
}

}  // namespace


void OplogWriterStats::incrementBatchSize(uint64_t n) {
    _batchSize.increment(n);
}

TimerStats& OplogWriterStats::getBatches() {
    return _batches;
}

BSONObj OplogWriterStats::getReport() const {
    BSONObjBuilder b;
    b.append("batchSize", _batchSize.get());
    b.append("batches", _batches.getReport());
    return b.obj();
}

OplogWriterImpl::OplogWriterImpl(executor::TaskExecutor* executor,
                                 OplogBuffer* writeBuffer,
                                 OplogBuffer* applyBuffer,
                                 ReplicationCoordinator* replCoord,
                                 StorageInterface* storageInterface,
                                 Observer* observer,
                                 const OplogWriter::Options& options)
    : OplogWriter(executor, writeBuffer, options),
      _applyBuffer(applyBuffer),
      _replCoord(replCoord),
      _storageInterface(storageInterface),
      _observer(observer) {}

void OplogWriterImpl::_run() {
    // We don't start data replication for arbiters at all and it's not allowed to reconfig
    // arbiterOnly field for any member.
    invariant(!_replCoord->getMemberState().arbiter());

    const auto flushJournal = !getGlobalServiceContext()->getStorageEngine()->isEphemeral();
    const auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();

    // Oplog writes are crucial to the stability of the replica set. We give the operations
    // Immediate priority so that it skips waiting for ticket acquisition and flow control.
    ScopedAdmissionPriority priority(opCtx, AdmissionContext::Priority::kExempt);

    while (true) {
        // For pausing replication in tests.
        if (MONGO_unlikely(rsSyncApplyStop.shouldFail())) {
            LOGV2(8543102,
                  "Oplog Writer - rsSyncApplyStop fail point enabled. Blocking until fail "
                  "point is disabled");
            rsSyncApplyStop.pauseWhileSet(opCtx);
        }

        // Transition to SECONDARY state, if possible.
        // TODO (SERVER-87675): investigate if this should be called here.
        _replCoord->finishRecoveryIfEligible(opCtx);

        auto batch = _batcher.getNextBatch(opCtx, Seconds(1));

        // Signal the apply buffer to enter or exit drain mode if it is not.
        if (_applyBufferInDrainMode != batch.exhausted()) {
            if (_applyBufferInDrainMode) {
                _applyBuffer->exitDrainMode();
            } else {
                _applyBuffer->enterDrainMode();
            }
            _applyBufferInDrainMode = batch.exhausted();
        }

        if (batch.empty()) {
            if (inShutdown()) {
                return;
            }
            continue;
        }

        // Extract the opTime and wallTime of the last op in the batch.
        auto ops = batch.releaseBatch();
        auto lastOpTimeAndWallTime =
            invariantStatusOK(OpTimeAndWallTime::parseOpTimeAndWallTimeFromOplogEntry(ops.back()));

        // Write the operations in this batch. 'writeOplogBatch' returns the optime of
        // the last op that was written, which should be the last optime in the batch.
        auto swLastOpTime = writeOplogBatch(opCtx, ops);
        if (swLastOpTime.getStatus().code() == ErrorCodes::InterruptedAtShutdown) {
            return;
        }
        fassertNoTrace(8543103, swLastOpTime);
        invariant(swLastOpTime.getValue() == lastOpTimeAndWallTime.opTime);

        // Update various things that care about our last written optime.
        finalizeOplogBatch(opCtx, lastOpTimeAndWallTime, flushJournal);

        // Push the entries to the applier's buffer, may be blocked if buffer is full.
        _applyBuffer->push(opCtx, ops.begin(), ops.end());
    }
}

StatusWith<OpTime> OplogWriterImpl::writeOplogBatch(OperationContext* opCtx,
                                                    const std::vector<BSONObj>& ops) {
    invariant(!ops.empty());
    LOGV2_DEBUG(8352100, 2, "Oplog write batch size", "size"_attr = ops.size());

    bool writeOplogColl = !getOptions().skipWritesToOplogColl;
    bool writeChangeColl = change_stream_serverless_helpers::isChangeCollectionsModeActive();

    // This should only happen for recovery modes.
    if (!writeOplogColl && !writeChangeColl) {
        return OpTime();
    }

    // Increment the batch stats.
    oplogWriterMetric.incrementBatchSize(ops.size());
    TimerHolder timer(&oplogWriterMetric.getBatches());

    // Create insert statements from the oplog entries.
    std::vector<InsertStatement> docs;
    docs.reserve(ops.size());

    for (const auto& op : ops) {
        auto opTime = invariantStatusOK(OpTime::parseFromOplogEntry(op));
        docs.emplace_back(InsertStatement{op, opTime.getTimestamp(), opTime.getTerm()});
    }

    // Perform writes to oplog collection and/or change collection.
    _writeOplogBatchImpl(opCtx, docs, writeOplogColl, writeChangeColl);

    return docs.back().oplogSlot;
}

void OplogWriterImpl::finalizeOplogBatch(OperationContext* opCtx,
                                         const OpTimeAndWallTime& lastOpTimeAndWallTime,
                                         bool flushJournal) {
    // 1. Update oplog visibility by notifying the storage engine of the latest opTime.
    _storageInterface->oplogDiskLocRegister(
        opCtx, lastOpTimeAndWallTime.opTime.getTimestamp(), true /* orderedCommit */);

    // 2. Advance the lastWritten opTime to the last opTime in batch.
    _replCoord->setMyLastWrittenOpTimeAndWallTimeForward(lastOpTimeAndWallTime);

    // 3. Trigger the journal flusher.
    // This should be done after the lastWritten opTime is advanced because the journal
    // flusher will first read lastWritten and later advance lastDurable to lastWritten
    // upon finish.
    if (flushJournal) {
        JournalFlusher::get(opCtx)->triggerJournalFlush();
    }
}

void OplogWriterImpl::_writeOplogBatchImpl(OperationContext* opCtx,
                                           const std::vector<InsertStatement>& docs,
                                           bool writeOplogColl,
                                           bool writeChangeColl) {
    // Oplog writes are crucial to the stability of the replica set. We give the operations
    // Immediate priority so that it skips waiting for ticket acquisition and flow control.
    ScopedAdmissionPriority priority(opCtx, AdmissionContext::Priority::kExempt);
    UnreplicatedWritesBlock uwb(opCtx);

    // The 'nsOrUUID' is used only to log the debug message when retrying inserts on the
    // oplog and change collections. The 'writeConflictRetry' helper assumes operations
    // are done on a single a single namespace. But the provided insert function can do
    // inserts on the oplog and/or multiple change collections, ie. multiple namespaces.
    // As such 'writeConflictRetry' will not log the correct namespace when retrying.
    NamespaceStringOrUUID nsOrUUID = writeOplogColl
        ? NamespaceString::kRsOplogNamespace
        : NamespaceString::makeChangeCollectionNSS(boost::none /* tenantId */);

    fassert(8792300,
            storage_helpers::insertBatchAndHandleRetry(
                opCtx, nsOrUUID, docs, [&](auto* opCtx, auto begin, auto end) {
                    return insertDocsToOplogAndChangeCollections(
                        opCtx, begin, end, writeOplogColl, writeChangeColl, _observer);
                }));
}

}  // namespace repl
}  // namespace mongo
