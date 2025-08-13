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

#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/repl/initial_sync/initial_syncer.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/db/storage/storage_util.h"
#include "mongo/stdx/mutex.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {

namespace {

constexpr size_t kMinOpsPerThread = 16;

auto checkFeatureFlagReduceMajorityWriteLatencyFn = [] {
    return feature_flags::gReduceMajorityWriteLatency.isEnabled();
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
    boost::optional<AutoGetOplogFastPath> autoOplog;
    boost::optional<ChangeStreamChangeCollectionManager::ChangeCollectionsWriter> ccw;

    // Acquire locks. We must acquire the locks for all collections we intend to write to before
    // performing any writes. This avoids potential deadlocks created by waiting for locks while
    // having generated oplog holes.
    if (writeOplogColl) {
        autoOplog.emplace(
            opCtx,
            OplogAccessMode::kWrite,
            Date_t::max(),
            AutoGetOplogFastPathOptions{.explicitIntent =
                                            rss::consensus::IntentRegistry::Intent::LocalWrite});
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

        const size_t count = std::distance(begin, end);
        std::vector<Record> records;
        records.reserve(count);
        std::vector<Timestamp> timestamps;
        timestamps.reserve(count);
        for (auto it = begin; it != end; it++) {
            const auto& doc = it->doc;
            records.emplace_back(Record{RecordId(), RecordData(doc.objdata(), doc.objsize())});
            timestamps.emplace_back(it->oplogSlot.getTimestamp());
        }

        auto status = internal::insertDocumentsForOplog(opCtx, oplogColl, &records, timestamps);
        if (!status.isOK()) {
            return status;
        }
        opCtx->getServiceContext()->getOpObserver()->onInserts(opCtx,
                                                               oplogColl,
                                                               begin,
                                                               end,
                                                               /*recordIds=*/{},
                                                               /*fromMigrate=*/
                                                               std::vector(count, false),
                                                               /*defaultFromMigrate=*/false);
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

std::vector<InsertStatement> makeInsertStatements(const std::vector<BSONObj>& ops,
                                                  size_t begin,
                                                  size_t end) {
    std::vector<InsertStatement> docs;
    docs.reserve(end - begin);

    for (size_t i = begin; i < end; ++i) {
        auto opTime = invariantStatusOK(OpTime::parseFromOplogEntry(ops[i]));
        docs.emplace_back(ops[i], opTime.getTimestamp(), opTime.getTerm());
    }

    return docs;
}

std::vector<InsertStatement> makeInsertStatements(const std::vector<repl::OplogEntry>& ops,
                                                  size_t begin,
                                                  size_t end) {
    std::vector<InsertStatement> docs;
    docs.reserve(end - begin);

    for (size_t i = begin; i < end; ++i) {
        docs.emplace_back(ops[i].getEntry().getRaw(),
                          ops[i].getOpTime().getTimestamp(),
                          ops[i].getOpTime().getTerm());
    }

    return docs;
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
                                 ThreadPool* workerPool,
                                 ReplicationCoordinator* replCoord,
                                 StorageInterface* storageInterface,
                                 ReplicationConsistencyMarkers* consistencyMarkers,
                                 Observer* observer,
                                 const OplogWriter::Options& options)
    : OplogWriter(executor, writeBuffer, options),
      _applyBuffer(applyBuffer),
      _workerPool(workerPool),
      _replCoord(replCoord),
      _storageInterface(storageInterface),
      _consistencyMarkers(consistencyMarkers),
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
    ScopedAdmissionPriority<ExecutionAdmissionContext> priority(
        opCtx, AdmissionContext::Priority::kExempt);

    while (true) {
        // For pausing replication in tests.
        if (MONGO_unlikely(rsSyncApplyStop.shouldFail())) {
            LOGV2(8543102,
                  "Oplog Writer - rsSyncApplyStop fail point enabled. Blocking until fail "
                  "point is disabled");
            rsSyncApplyStop.pauseWhileSet(opCtx);
        }

        auto batch = _batcher.getNextBatch(opCtx, Seconds(1));
        if (batch.empty()) {
            if (inShutdown()) {
                return;
            }
            if (batch.termWhenExhausted()) {
                // The writer's buffer has been drained, now signal the applier's buffer
                // to enter drain mode.
                _replCoord->signalWriterDrainComplete(opCtx, *batch.termWhenExhausted());
            }
            continue;
        }

        // Extract the opTime and wallTime of the last op in the batch.
        auto ops = batch.releaseBatch();
        auto lastOpTimeAndWallTime =
            invariantStatusOK(OpTimeAndWallTime::parseOpTimeAndWallTimeFromOplogEntry(ops.back()));

        stdx::lock_guard<stdx::mutex> fsynclk(oplogWriterLockedFsync);

        {
            LOGV2_DEBUG(8352100, 2, "Oplog write batch size", "size"_attr = ops.size());

            // Increment the batch stats.
            oplogWriterMetric.incrementBatchSize(ops.size());
            TimerHolder timer(&oplogWriterMetric.getBatches());

            // Write the operations in this batch.
            invariant(writeOplogBatch(opCtx, ops));
        }

        // Update various things that care about our last written optime.
        finalizeOplogBatch(opCtx, lastOpTimeAndWallTime, flushJournal);

        // Push the entries to the applier's buffer, may be blocked if no enough space.
        _applyBuffer->push(opCtx, ops.begin(), ops.end());
    }
}

bool OplogWriterImpl::writeOplogBatch(OperationContext* opCtx, const std::vector<BSONObj>& ops) {
    invariant(!ops.empty());

    // Don't do anything if not writing to the oplog collection nor the change collections.
    auto [writeOplogColl, writeChangeColl] =
        _checkWriteOptions(VersionContext::getDecoration(opCtx));
    if (!writeOplogColl && !writeChangeColl) {
        return false;
    }

    // Write to the oplog and/or change collections in the same storage transaction.
    _writeOplogBatchForRange(opCtx, ops, 0, ops.size(), writeOplogColl, writeChangeColl);

    return true;
}

bool OplogWriterImpl::scheduleWriteOplogBatch(OperationContext* opCtx,
                                              const std::vector<OplogEntry>& ops) {
    invariant(!ops.empty());

    // Don't do anything if not writing to the oplog collection nor the change collections.
    auto [writeOplogColl, writeChangeColl] =
        _checkWriteOptions(VersionContext::getDecoration(opCtx));
    if (!writeOplogColl && !writeChangeColl) {
        return false;
    }

    // Write to the oplog collection and/or change collections using the thread pool.

    // When performing writes with multiple threads, we must set oplogTruncateAfterPoint
    // in case the server crashes before all the threads finish. In such cases the oplog
    // will be truncated after this opTime during startup recovery in order to make sure
    // there are no holes in the oplog.
    if (writeOplogColl) {
        _consistencyMarkers->setOplogTruncateAfterPoint(
            opCtx, _replCoord->getMyLastWrittenOpTime().getTimestamp());
    }

    auto makeOplogWriteForRange = [this,
                                   &ops,
                                   writeOplogColl = writeOplogColl,
                                   writeChangeColl = writeChangeColl](size_t begin, size_t end) {
        return [=, this, &ops](auto status) {
            invariant(status);
            auto opCtx = cc().makeOperationContext();
            _writeOplogBatchForRange(opCtx.get(), ops, begin, end, writeOplogColl, writeChangeColl);
        };
    };

    // We want to be able to take advantage of bulk inserts so we don't use multiple threads
    // if it would result too little work per thread. This also ensures that we can amortize
    // the setup/teardown overhead across many writes.
    invariant(_workerPool);
    const auto poolMaxThreads = _workerPool->getStats().options.maxThreads;
    const auto enoughToMultiThread = ops.size() >= kMinOpsPerThread * poolMaxThreads;
    const auto numWriteThreads = enoughToMultiThread ? poolMaxThreads : 1;
    const size_t numOpsPerThread = ops.size() / numWriteThreads;

    // Schedule the writes to the thread pool, do not wait for finish.
    for (size_t t = 0; t < numWriteThreads; ++t) {
        size_t begin = t * numOpsPerThread;
        size_t end = (t == numWriteThreads - 1) ? ops.size() : begin + numOpsPerThread;
        _workerPool->schedule(makeOplogWriteForRange(begin, end));
    }

    return true;
}

void OplogWriterImpl::waitForScheduledWrites(OperationContext* opCtx) {
    // Wait for all scheduled writes to complete.
    _workerPool->waitForIdle();

    // Reset oplogTruncateAfterPoint after writes are complete.
    bool writeOplogColl = !getOptions().skipWritesToOplogColl;
    if (writeOplogColl) {
        _consistencyMarkers->setOplogTruncateAfterPoint(opCtx, Timestamp());
    }
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

std::pair<bool, bool> OplogWriterImpl::_checkWriteOptions(const VersionContext& vCtx) {
    bool writeOplogColl = !getOptions().skipWritesToOplogColl;
    bool writeChangeColl = !getOptions().skipWritesToChangeColl &&
        change_stream_serverless_helpers::isChangeCollectionsModeActive(vCtx);

    return {writeOplogColl, writeChangeColl};
}

template <typename T>
void OplogWriterImpl::_writeOplogBatchForRange(OperationContext* opCtx,
                                               const std::vector<T>& ops,
                                               size_t begin,
                                               size_t end,
                                               bool writeOplogColl,
                                               bool writeChangeColl) {
    // Oplog writes are crucial to the stability of the replica set. We give the operations
    // Immediate priority so that it skips waiting for ticket acquisition and flow control.
    ScopedAdmissionPriority<ExecutionAdmissionContext> priority(
        opCtx, AdmissionContext::Priority::kExempt);
    UnreplicatedWritesBlock uwb(opCtx);

    auto docs = makeInsertStatements(ops, begin, end);

    // The 'nsOrUUID' is used only to log the debug message when retrying inserts on the
    // oplog and change collections. The 'writeConflictRetry' helper assumes operations
    // are done on a single namespace. But the provided insert function can do
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
