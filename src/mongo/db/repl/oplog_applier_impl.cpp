/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/repl/oplog_applier_impl.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/oplog_applier_utils.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/transaction_oplog_application.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/db/storage/storage_util.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/basic.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log_with_sampling.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {
namespace {

MONGO_FAIL_POINT_DEFINE(pauseBatchApplicationBeforeCompletion);
MONGO_FAIL_POINT_DEFINE(pauseBatchApplicationAfterWritingOplogEntries);
MONGO_FAIL_POINT_DEFINE(hangAfterRecordingOpApplicationStartTime);

// The oplog entries applied
CounterMetric opsAppliedStats("repl.apply.ops");

// Tracks the oplog application batch size.
CounterMetric oplogApplicationBatchSize("repl.apply.batchSize");

// Number and time of each ApplyOps worker pool round
auto& applyBatchStats = makeServerStatusMetric<TimerStats>("repl.apply.batches");

/**
 * Used for logging a report of ops that take longer than "slowMS" to apply. This is called
 * right before returning from applyOplogEntryOrGroupedInserts, and it returns the same status.
 */
Status finishAndLogApply(OperationContext* opCtx,
                         ClockSource* clockSource,
                         Status finalStatus,
                         Date_t applyStartTime,
                         const OplogEntryOrGroupedInserts& entryOrGroupedInserts) {

    if (finalStatus.isOK()) {
        auto applyEndTime = clockSource->now();
        auto opDuration = durationCount<Milliseconds>(applyEndTime - applyStartTime);

        if (shouldLogSlowOpWithSampling(opCtx,
                                        MONGO_LOGV2_DEFAULT_COMPONENT,
                                        Milliseconds(opDuration),
                                        Milliseconds(serverGlobalParams.slowMS))
                .first) {

            logv2::DynamicAttributes attrs;

            auto redacted = redact(entryOrGroupedInserts.toBSON());
            if (entryOrGroupedInserts.getOp().getOpType() == OpTypeEnum::kCommand) {
                attrs.add("command", redacted);
            } else {
                attrs.add("CRUD", redacted);
            }

            attrs.add("duration", Milliseconds(opDuration));

            LOGV2(51801, "Applied op", attrs);
        }
    }
    return finalStatus;
}

void _addOplogChainOpsToWriterVectors(OperationContext* opCtx,
                                      std::vector<OplogEntry*>* partialTxnList,
                                      std::vector<std::vector<OplogEntry>>* derivedOps,
                                      OplogEntry* op,
                                      CachedCollectionProperties* collPropertiesCache,
                                      std::vector<std::vector<const OplogEntry*>>* writerVectors) {
    std::vector<OplogEntry> txnOps;
    bool shouldSerialize = false;
    std::tie(txnOps, shouldSerialize) =
        readTransactionOperationsFromOplogChainAndCheckForCommands(opCtx, *op, *partialTxnList);
    derivedOps->emplace_back(txnOps);
    partialTxnList->clear();

    // Transaction entries cannot have different session updates.
    OplogApplierUtils::addDerivedOps(
        opCtx, &derivedOps->back(), writerVectors, collPropertiesCache, shouldSerialize);
}

Status _insertDocumentsToOplogAndChangeCollections(
    OperationContext* opCtx,
    std::vector<InsertStatement>::const_iterator begin,
    std::vector<InsertStatement>::const_iterator end,
    bool skipWritesToOplog) {
    WriteUnitOfWork wunit(opCtx);

    if (!skipWritesToOplog) {
        AutoGetOplog autoOplog(opCtx, OplogAccessMode::kWrite);
        auto& oplogColl = autoOplog.getCollection();
        if (!oplogColl) {
            return {ErrorCodes::NamespaceNotFound, "Oplog collection does not exist"};
        }

        auto status = collection_internal::insertDocuments(
            opCtx, oplogColl, begin, end, nullptr /* OpDebug */, false /* fromMigrate */);
        if (!status.isOK()) {
            return status;
        }
    }

    // Write the corresponding oplog entries to tenants respective change
    // collections in the serverless.
    if (ChangeStreamChangeCollectionManager::isChangeCollectionsModeActive()) {
        auto status =
            ChangeStreamChangeCollectionManager::get(opCtx).insertDocumentsToChangeCollection(
                opCtx,
                begin,
                end,
                !skipWritesToOplog /* hasAcquiredGlobalIXLock */,
                nullptr /* OpDebug */);
        if (!status.isOK()) {
            return status;
        }
    }

    wunit.commit();

    return Status::OK();
}

}  // namespace


namespace {

class ApplyBatchFinalizer {
public:
    ApplyBatchFinalizer(ReplicationCoordinator* replCoord) : _replCoord(replCoord) {}
    virtual ~ApplyBatchFinalizer(){};

    virtual void record(const OpTimeAndWallTime& newOpTimeAndWallTime) {
        _recordApplied(newOpTimeAndWallTime);
    };

protected:
    void _recordApplied(const OpTimeAndWallTime& newOpTimeAndWallTime) {
        // We have to use setMyLastAppliedOpTimeAndWallTimeForward since this thread races with
        // ReplicationExternalStateImpl::onTransitionToPrimary.
        _replCoord->setMyLastAppliedOpTimeAndWallTimeForward(newOpTimeAndWallTime);
        // We know we're at a no-holes point and we've already advanced visibility; we need
        // to notify waiters since we changed the lastAppliedSnapshot.
        signalOplogWaiters();
    }

    void _recordDurable(const OpTimeAndWallTime& newOpTimeAndWallTime) {
        // We have to use setMyLastDurableOpTimeAndWallTimeForward since this thread races with
        // ReplicationExternalStateImpl::onTransitionToPrimary.
        _replCoord->setMyLastDurableOpTimeAndWallTimeForward(newOpTimeAndWallTime);
    }

private:
    // Used to update the replication system's progress.
    ReplicationCoordinator* _replCoord;
};

class ApplyBatchFinalizerForJournal : public ApplyBatchFinalizer {
public:
    ApplyBatchFinalizerForJournal(ReplicationCoordinator* replCoord)
        : ApplyBatchFinalizer(replCoord),
          _waiterThread{&ApplyBatchFinalizerForJournal::_run, this} {};
    ~ApplyBatchFinalizerForJournal();

    void record(const OpTimeAndWallTime& newOpTimeAndWallTime) override;

private:
    /**
     * Loops continuously, waiting for writes to be flushed to disk and then calls
     * ReplicationCoordinator::setMyLastOptime with _latestOpTime.
     * Terminates once _shutdownSignaled is set true.
     */
    void _run();

    // Protects _cond, _shutdownSignaled, and _latestOpTime.
    Mutex _mutex = MONGO_MAKE_LATCH("OplogApplierImpl::_mutex");
    // Used to alert our thread of a new OpTime.
    stdx::condition_variable _cond;
    // The next OpTime to set as the ReplicationCoordinator's lastOpTime after flushing.
    OpTimeAndWallTime _latestOpTimeAndWallTime;
    // Once this is set to true the _run method will terminate.
    bool _shutdownSignaled = false;
    // Thread that will _run(). Must be initialized last as it depends on the other variables.
    stdx::thread _waiterThread;
};

ApplyBatchFinalizerForJournal::~ApplyBatchFinalizerForJournal() {
    stdx::unique_lock<Latch> lock(_mutex);
    _shutdownSignaled = true;
    _cond.notify_all();
    lock.unlock();

    _waiterThread.join();
}

void ApplyBatchFinalizerForJournal::record(const OpTimeAndWallTime& newOpTimeAndWallTime) {
    _recordApplied(newOpTimeAndWallTime);

    stdx::unique_lock<Latch> lock(_mutex);
    _latestOpTimeAndWallTime = newOpTimeAndWallTime;
    _cond.notify_all();
}

void ApplyBatchFinalizerForJournal::_run() {
    Client::initThread("ApplyBatchFinalizerForJournal");

    while (true) {
        OpTimeAndWallTime latestOpTimeAndWallTime = {OpTime(), Date_t()};

        {
            stdx::unique_lock<Latch> lock(_mutex);
            while (_latestOpTimeAndWallTime.opTime.isNull() && !_shutdownSignaled) {
                _cond.wait(lock);
            }

            if (_shutdownSignaled) {
                return;
            }

            latestOpTimeAndWallTime = _latestOpTimeAndWallTime;
            _latestOpTimeAndWallTime = {OpTime(), Date_t()};
        }

        auto opCtx = cc().makeOperationContext();
        JournalFlusher::get(opCtx.get())->waitForJournalFlush();
        _recordDurable(latestOpTimeAndWallTime);
    }
}

}  // namespace

OplogApplierImpl::OplogApplierImpl(executor::TaskExecutor* executor,
                                   OplogBuffer* oplogBuffer,
                                   Observer* observer,
                                   ReplicationCoordinator* replCoord,
                                   ReplicationConsistencyMarkers* consistencyMarkers,
                                   StorageInterface* storageInterface,
                                   const OplogApplier::Options& options,
                                   ThreadPool* writerPool)
    : OplogApplier(executor, oplogBuffer, observer, options),
      _replCoord(replCoord),
      _writerPool(writerPool),
      _storageInterface(storageInterface),
      _consistencyMarkers(consistencyMarkers),
      _beginApplyingOpTime(options.beginApplyingOpTime) {}

void OplogApplierImpl::_run(OplogBuffer* oplogBuffer) {
    // Start up a thread from the batcher to pull from the oplog buffer into the batcher's oplog
    // batch.
    _oplogBatcher->startup(_storageInterface);

    ON_BLOCK_EXIT([this] { _oplogBatcher->shutdown(); });

    // We don't start data replication for arbiters at all and it's not allowed to reconfig
    // arbiterOnly field for any member.
    invariant(!_replCoord->getMemberState().arbiter());

    std::unique_ptr<ApplyBatchFinalizer> finalizer{
        getGlobalServiceContext()->getStorageEngine()->isEphemeral()
            ? new ApplyBatchFinalizer(_replCoord)
            : new ApplyBatchFinalizerForJournal(_replCoord)};

    while (true) {  // Exits on message from OplogBatcher.
        // Use a new operation context each iteration, as otherwise we may appear to use a single
        // collection name to refer to collections with different UUIDs.
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;

        // This code path gets used during elections, so it should not be subject to Flow Control.
        // It is safe to exclude this operation context from Flow Control here because this code
        // path only gets used on secondaries or on a node transitioning to primary.
        opCtx.setShouldParticipateInFlowControl(false);

        // For pausing replication in tests.
        if (MONGO_unlikely(rsSyncApplyStop.shouldFail())) {
            LOGV2(21229,
                  "Oplog Applier - rsSyncApplyStop fail point enabled. Blocking until fail "
                  "point is disabled");
            rsSyncApplyStop.pauseWhileSet(&opCtx);
        }

        // Transition to SECONDARY state, if possible.
        _replCoord->finishRecoveryIfEligible(&opCtx);

        // Blocks up to a second waiting for a batch to be ready to apply. If one doesn't become
        // ready in time, we'll loop again so we can do the above checks periodically.
        OplogBatch ops = _oplogBatcher->getNextBatch(Seconds(1));
        if (ops.empty()) {
            if (ops.mustShutdown()) {
                // Shut down and exit oplog application loop.
                return;
            }
            if (MONGO_unlikely(rsSyncApplyStop.shouldFail())) {
                continue;
            }
            if (ops.termWhenExhausted()) {
                // Signal drain complete if we're in Draining state and the buffer is empty.
                // Since we check the states of batcher and oplog buffer without synchronization,
                // they can be stale. We make sure the applier is still draining in the given term
                // before and after the check, so that if the oplog buffer was exhausted, then
                // it still will be.
                _replCoord->signalDrainComplete(&opCtx, *ops.termWhenExhausted());
            }
            continue;  // Try again.
        }

        // Extract some info from ops that we'll need after releasing the batch below.
        const auto firstOpTimeInBatch = ops.front().getOpTime();
        const auto lastOpInBatch = ops.back();
        const auto lastOpTimeInBatch = lastOpInBatch.getOpTime();
        const auto lastWallTimeInBatch = lastOpInBatch.getWallClockTime();
        const auto lastAppliedOpTimeAtStartOfBatch = _replCoord->getMyLastAppliedOpTime();

        // Make sure the oplog doesn't go back in time or repeat an entry.
        if (firstOpTimeInBatch <= lastAppliedOpTimeAtStartOfBatch) {
            fassert(34361,
                    Status(ErrorCodes::OplogOutOfOrder,
                           str::stream() << "Attempted to apply an oplog entry ("
                                         << firstOpTimeInBatch.toString()
                                         << ") which is not greater than our last applied OpTime ("
                                         << lastAppliedOpTimeAtStartOfBatch.toString() << ")."));
        }

        // Don't allow the fsync+lock thread to see intermediate states of batch application.
        stdx::lock_guard<SimpleMutex> fsynclk(filesLockedFsync);

        // Apply the operations in this batch. '_applyOplogBatch' returns the optime of the
        // last op that was applied, which should be the last optime in the batch.
        auto swLastOpTimeAppliedInBatch = _applyOplogBatch(&opCtx, ops.releaseBatch());
        if (swLastOpTimeAppliedInBatch.getStatus().code() == ErrorCodes::InterruptedAtShutdown) {
            // If an operation was interrupted at shutdown, fail the batch without advancing
            // appliedThrough as if this were an unclean shutdown. This ensures the stable timestamp
            // does not advance, and a checkpoint cannot be taken at a timestamp that includes this
            // batch. On startup, we will recover from an earlier stable checkpoint and apply the
            // operations from this batch again.
            return;
        }
        fassertNoTrace(34437, swLastOpTimeAppliedInBatch);
        invariant(swLastOpTimeAppliedInBatch.getValue() == lastOpTimeInBatch);

        // Update various things that care about our last applied optime.

        // 1. Ensure that the last applied op time hasn't changed since the start of this batch.
        const auto lastAppliedOpTimeAtEndOfBatch = _replCoord->getMyLastAppliedOpTime();
        invariant(lastAppliedOpTimeAtStartOfBatch == lastAppliedOpTimeAtEndOfBatch,
                  str::stream() << "the last known applied OpTime has changed from "
                                << lastAppliedOpTimeAtStartOfBatch.toString() << " to "
                                << lastAppliedOpTimeAtEndOfBatch.toString()
                                << " in the middle of batch application");

        // 2. Update oplog visibility by notifying the storage engine of the new oplog entries.
        const bool orderedCommit = true;
        _storageInterface->oplogDiskLocRegister(
            &opCtx, lastOpTimeInBatch.getTimestamp(), orderedCommit);

        // 3. Finalize this batch. The finalizer advances the global timestamp to lastOpTimeInBatch.
        finalizer->record({lastOpTimeInBatch, lastWallTimeInBatch});
    }
}


// Schedules the writes to the oplog and the change collection for 'ops' into threadPool. The caller
// must guarantee that 'ops' stays valid until all scheduled work in the thread pool completes.
void scheduleWritesToOplogAndChangeCollection(OperationContext* opCtx,
                                              StorageInterface* storageInterface,
                                              ThreadPool* writerPool,
                                              const std::vector<OplogEntry>& ops,
                                              bool skipWritesToOplog) {
    // Skip performing any writes during the startup recovery when running in the non-serverless
    // environment.
    if (skipWritesToOplog &&
        !ChangeStreamChangeCollectionManager::isChangeCollectionsModeActive()) {
        return;
    }

    auto makeOplogWriterForRange = [storageInterface, &ops, skipWritesToOplog](size_t begin,
                                                                               size_t end) {
        // The returned function will be run in a separate thread after this returns. Therefore
        // all captures other than 'ops' must be by value since they will not be available. The
        // caller guarantees that 'ops' will stay in scope until the spawned threads complete.
        return [storageInterface, &ops, begin, end, skipWritesToOplog](auto status) {
            invariant(status);
            auto opCtx = cc().makeOperationContext();

            // This code path is only executed on secondaries and initial syncing nodes, so it is
            // safe to exclude any writes from Flow Control.
            opCtx->setShouldParticipateInFlowControl(false);

            UnreplicatedWritesBlock uwb(opCtx.get());
            ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(
                opCtx->lockState());

            std::vector<InsertStatement> docs;
            docs.reserve(end - begin);
            for (size_t i = begin; i < end; i++) {
                docs.emplace_back(InsertStatement{ops[i].getEntry().getRaw(),
                                                  ops[i].getOpTime().getTimestamp(),
                                                  ops[i].getOpTime().getTerm()});
            }

            // TODO SERVER-67168 the 'nsOrUUID' is used only to log the debug message when retrying
            // inserts on the oplog and change collections. The 'writeConflictRetry' assumes
            // operations are done on a single namespace. But the method
            // '_insertDocumentsToOplogAndChangeCollections' can perform inserts on the oplog and
            // multiple change collections, ie. several namespaces. As such 'writeConflictRetry'
            // will not log the correct namespace when retrying. Refactor this code to log the
            // correct namespace in the log message.
            NamespaceStringOrUUID nsOrUUID = !skipWritesToOplog
                ? NamespaceString::kRsOplogNamespace
                : NamespaceString::makeChangeCollectionNSS(boost::none /* tenantId */);

            fassert(6663400,
                    storage_helpers::insertBatchAndHandleRetry(
                        opCtx.get(), nsOrUUID, docs, [&](auto* opCtx, auto begin, auto end) {
                            return _insertDocumentsToOplogAndChangeCollections(
                                opCtx, begin, end, skipWritesToOplog);
                        }));
        };
    };

    // We want to be able to take advantage of bulk inserts so we don't use multiple threads if it
    // would result too little work per thread. This also ensures that we can amortize the
    // setup/teardown overhead across many writes.
    const size_t kMinOplogEntriesPerThread = 16;
    const bool enoughToMultiThread =
        ops.size() >= kMinOplogEntriesPerThread * writerPool->getStats().options.maxThreads;

    // Storage engines support parallel writes to the oplog because they are required to ensure that
    // oplog entries are ordered correctly, even if inserted out-of-order.
    if (!enoughToMultiThread) {
        writerPool->schedule(makeOplogWriterForRange(0, ops.size()));
        return;
    }


    const size_t numOplogThreads = writerPool->getStats().options.maxThreads;
    const size_t numOpsPerThread = ops.size() / numOplogThreads;
    for (size_t thread = 0; thread < numOplogThreads; thread++) {
        size_t begin = thread * numOpsPerThread;
        size_t end = (thread == numOplogThreads - 1) ? ops.size() : begin + numOpsPerThread;
        writerPool->schedule(makeOplogWriterForRange(begin, end));
    }
}

StatusWith<OpTime> OplogApplierImpl::_applyOplogBatch(OperationContext* opCtx,
                                                      std::vector<OplogEntry> ops) {
    invariant(!ops.empty());

    LOGV2_DEBUG(21230,
                2,
                "replication batch size is {size}",
                "Replication batch size",
                "size"_attr = ops.size());

    // Stop all readers until we're done. This also prevents doc-locking engines from deleting old
    // entries from the oplog until we finish writing.
    Lock::ParallelBatchWriterMode pbwm(opCtx->lockState());

    invariant(_replCoord);
    if (_replCoord->getApplierState() == ReplicationCoordinator::ApplierState::Stopped) {
        LOGV2_FATAL_CONTINUE(21234, "Attempting to replicate ops while primary");
        return {ErrorCodes::CannotApplyOplogWhilePrimary,
                "attempting to replicate ops while primary"};
    }

    // Increment the batch size stat.
    oplogApplicationBatchSize.increment(ops.size());

    std::vector<WorkerMultikeyPathInfo> multikeyVector(_writerPool->getStats().options.maxThreads);
    {
        // Each node records cumulative batch application stats for itself using this timer.
        TimerHolder timer(&applyBatchStats);

        // We must wait for the all work we've dispatched to complete before leaving this block
        // because the spawned threads refer to objects on the stack
        ON_BLOCK_EXIT([&] { _writerPool->waitForIdle(); });

        // Write batch of ops into oplog.
        if (!getOptions().skipWritesToOplog) {
            _consistencyMarkers->setOplogTruncateAfterPoint(
                opCtx, _replCoord->getMyLastAppliedOpTime().getTimestamp());
        }

        scheduleWritesToOplogAndChangeCollection(
            opCtx, _storageInterface, _writerPool, ops, getOptions().skipWritesToOplog);

        // Holds 'pseudo operations' generated by secondaries to aid in replication.
        // Keep in scope until all operations in 'ops' and 'derivedOps' have been applied.
        // Pseudo operations include:
        // - applyOps operations expanded to individual ops.
        // - ops to update config.transactions. Normal writes to config.transactions in the
        //   primary don't create an oplog entry, so extract info from writes with transactions
        //   and create a pseudo oplog.
        std::vector<std::vector<OplogEntry>> derivedOps;

        std::vector<std::vector<const OplogEntry*>> writerVectors(
            _writerPool->getStats().options.maxThreads);
        fillWriterVectors(opCtx, &ops, &writerVectors, &derivedOps);

        // Wait for writes to finish before applying ops.
        _writerPool->waitForIdle();

        // Use this fail point to hold the PBWM lock after we have written the oplog entries but
        // before we have applied them.
        if (MONGO_unlikely(pauseBatchApplicationAfterWritingOplogEntries.shouldFail())) {
            LOGV2(21231,
                  "pauseBatchApplicationAfterWritingOplogEntries fail point enabled. Blocking "
                  "until fail point is disabled");
            pauseBatchApplicationAfterWritingOplogEntries.pauseWhileSet(opCtx);
        }

        // Read `minValid` prior to it possibly being written to.
        const bool isDataConsistent =
            _consistencyMarkers->getMinValid(opCtx) < ops.front().getOpTime();

        // Reset consistency markers in case the node fails while applying ops.
        if (!getOptions().skipWritesToOplog) {
            _consistencyMarkers->setOplogTruncateAfterPoint(opCtx, Timestamp());
        }

        {

            std::vector<Status> statusVector(_writerPool->getStats().options.maxThreads,
                                             Status::OK());
            // Doles out all the work to the writer pool threads. writerVectors is not modified,
            // but  applyOplogBatchPerWorker will modify the vectors that it contains.
            invariant(writerVectors.size() == statusVector.size());
            for (size_t i = 0; i < writerVectors.size(); i++) {
                if (writerVectors[i].empty())
                    continue;

                _writerPool->schedule([this,
                                       &writer = writerVectors.at(i),
                                       &status = statusVector.at(i),
                                       &multikeyVector = multikeyVector.at(i),
                                       isDataConsistent = isDataConsistent](auto scheduleStatus) {
                    invariant(scheduleStatus);

                    auto opCtx = cc().makeOperationContext();

                    // This code path is only executed on secondaries and initial syncing nodes, so
                    // it is safe to exclude any writes from Flow Control.
                    opCtx->setShouldParticipateInFlowControl(false);
                    opCtx->setEnforceConstraints(false);

                    status = opCtx->runWithoutInterruptionExceptAtGlobalShutdown([&] {
                        return applyOplogBatchPerWorker(
                            opCtx.get(), &writer, &multikeyVector, isDataConsistent);
                    });
                });
            }

            _writerPool->waitForIdle();

            // If any of the statuses is not ok, return error.
            for (auto it = statusVector.cbegin(); it != statusVector.cend(); ++it) {
                const auto& status = *it;
                if (!status.isOK()) {
                    LOGV2_FATAL_CONTINUE(
                        21235,
                        "Failed to apply batch of operations. Number of operations in "
                        "batch: {numOperationsInBatch}. First operation: {firstOperation}. "
                        "Last operation: "
                        "{lastOperation}. Oplog application failed in writer thread "
                        "{failedWriterThread}: {error}",
                        "Failed to apply batch of operations",
                        "numOperationsInBatch"_attr = ops.size(),
                        "firstOperation"_attr = redact(ops.front().toBSONForLogging()),
                        "lastOperation"_attr = redact(ops.back().toBSONForLogging()),
                        "failedWriterThread"_attr = std::distance(statusVector.cbegin(), it),
                        "error"_attr = redact(status));
                    return status;
                }
            }
        }
    }

    // Use this fail point to hold the PBWM lock and prevent the batch from completing.
    if (MONGO_unlikely(pauseBatchApplicationBeforeCompletion.shouldFail())) {
        LOGV2(21232,
              "pauseBatchApplicationBeforeCompletion fail point enabled. Blocking until fail "
              "point is disabled");
        while (MONGO_unlikely(pauseBatchApplicationBeforeCompletion.shouldFail())) {
            if (inShutdown()) {
                LOGV2_FATAL_NOTRACE(
                    50798,
                    "Turn off pauseBatchApplicationBeforeCompletion before attempting "
                    "clean shutdown");
            }
            sleepmillis(100);
        }
    }

    Timestamp firstTimeInBatch = ops.front().getTimestamp();
    // Set any indexes to multikey that this batch ignored. This must be done while holding the
    // parallel batch writer mode lock.
    for (WorkerMultikeyPathInfo infoVector : multikeyVector) {
        for (MultikeyPathInfo info : infoVector) {
            // We timestamp every multikey write with the first timestamp in the batch. It is always
            // safe to set an index as multikey too early, just not too late. We conservatively pick
            // the first timestamp in the batch since we do not have enough information to find out
            // the timestamp of the first write that set the given multikey path.
            fassert(50686,
                    _storageInterface->setIndexIsMultikey(opCtx,
                                                          info.nss,
                                                          info.collectionUUID,
                                                          info.indexName,
                                                          info.multikeyMetadataKeys,
                                                          info.multikeyPaths,
                                                          firstTimeInBatch));
        }
    }

    // Increment the counter for the number of ops applied during catchup if the node is in catchup
    // mode.
    _replCoord->incrementNumCatchUpOpsIfCatchingUp(ops.size());

    // We have now written all database writes and updated the oplog to match.
    return ops.back().getOpTime();
}

/**
 * ops - This only modifies the isForCappedCollection field on each op. It does not alter the ops
 *      vector in any other way.
 * writerVectors - Set of operations for each worker thread to apply.
 * derivedOps - If provided, this function inserts a decomposition of applyOps operations
 *      and instructions for updating the transactions table.  Required if processing oplogs
 *      with transactions.
 * sessionUpdateTracker - if provided, keeps track of session info from ops.
 */
void OplogApplierImpl::_deriveOpsAndFillWriterVectors(
    OperationContext* opCtx,
    std::vector<OplogEntry>* ops,
    std::vector<std::vector<const OplogEntry*>>* writerVectors,
    std::vector<std::vector<OplogEntry>>* derivedOps,
    SessionUpdateTracker* sessionUpdateTracker) noexcept {

    LogicalSessionIdMap<std::vector<OplogEntry*>> partialTxnOps;
    CachedCollectionProperties collPropertiesCache;

    for (auto&& op : *ops) {
        // If the operation's optime is before or the same as the beginApplyingOpTime we don't want
        // to apply it, so don't include it in writerVectors.
        if (op.getOpTime() <= getOptions().beginApplyingOpTime) {
            continue;
        }

        // We need to track all types of ops, including type 'n' (these are generated from chunk
        // migrations).
        if (sessionUpdateTracker) {
            if (auto newOplogWrites = sessionUpdateTracker->updateSession(op)) {
                derivedOps->emplace_back(std::move(*newOplogWrites));
                OplogApplierUtils::addDerivedOps(opCtx,
                                                 &derivedOps->back(),
                                                 writerVectors,
                                                 &collPropertiesCache,
                                                 false /*serial*/);
            }
        }


        // If this entry is part of a multi-oplog-entry transaction, ignore it until the commit.
        // We must save it here because we are not guaranteed it has been written to the oplog
        // yet.
        // We also do this for prepare during initial sync.
        if (op.isPartialTransaction() ||
            (op.shouldPrepare() && getOptions().mode == OplogApplication::Mode::kInitialSync)) {
            auto& partialTxnList = partialTxnOps[*op.getSessionId()];
            // If this operation belongs to an existing partial transaction, partialTxnList
            // must contain the previous operations of the transaction.
            invariant(partialTxnList.empty() ||
                      partialTxnList.front()->getTxnNumber() == op.getTxnNumber());
            partialTxnList.push_back(&op);
            continue;
        }

        if (op.getCommandType() == OplogEntry::CommandType::kAbortTransaction) {
            auto& partialTxnList = partialTxnOps[*op.getSessionId()];
            partialTxnList.clear();
        }

        // Extract applyOps operations and fill writers with extracted operations using this
        // function.
        if (op.isTerminalApplyOps()) {
            auto logicalSessionId = op.getSessionId();
            // applyOps entries generated by a transaction must have a sessionId and a
            // transaction number.
            if (logicalSessionId && op.getTxnNumber()) {
                // On commit of unprepared transactions, get transactional operations from the
                // oplog and fill writers with those operations.
                // Flush partialTxnList operations for current transaction.
                auto& partialTxnList = partialTxnOps[*logicalSessionId];
                _addOplogChainOpsToWriterVectors(
                    opCtx, &partialTxnList, derivedOps, &op, &collPropertiesCache, writerVectors);
            } else {
                // The applyOps entry was not generated as part of a transaction.
                invariant(!op.getPrevWriteOpTimeInTransaction());

                derivedOps->emplace_back(ApplyOps::extractOperations(op));

                // Nested entries cannot have different session updates.
                OplogApplierUtils::addDerivedOps(opCtx,
                                                 &derivedOps->back(),
                                                 writerVectors,
                                                 &collPropertiesCache,
                                                 false /*serial*/);
            }
            continue;
        }

        // If we see a commitTransaction command that is a part of a prepared transaction during
        // initial sync, find the prepare oplog entry, extract applyOps operations, and fill writers
        // with the extracted operations.
        if (op.isPreparedCommit() && (getOptions().mode == OplogApplication::Mode::kInitialSync)) {
            auto logicalSessionId = op.getSessionId();
            auto& partialTxnList = partialTxnOps[*logicalSessionId];
            _addOplogChainOpsToWriterVectors(
                opCtx, &partialTxnList, derivedOps, &op, &collPropertiesCache, writerVectors);
            continue;
        }

        OplogApplierUtils::addToWriterVector(opCtx, &op, writerVectors, &collPropertiesCache);
    }
}

void OplogApplierImpl::fillWriterVectors(
    OperationContext* opCtx,
    std::vector<OplogEntry>* ops,
    std::vector<std::vector<const OplogEntry*>>* writerVectors,
    std::vector<std::vector<OplogEntry>>* derivedOps) noexcept {

    SessionUpdateTracker sessionUpdateTracker;
    _deriveOpsAndFillWriterVectors(opCtx, ops, writerVectors, derivedOps, &sessionUpdateTracker);

    auto newOplogWrites = sessionUpdateTracker.flushAll();
    if (!newOplogWrites.empty()) {
        derivedOps->emplace_back(std::move(newOplogWrites));
        _deriveOpsAndFillWriterVectors(
            opCtx, &derivedOps->back(), writerVectors, derivedOps, nullptr);
    }
}

void OplogApplierImpl::fillWriterVectors_forTest(
    OperationContext* opCtx,
    std::vector<OplogEntry>* ops,
    std::vector<std::vector<const OplogEntry*>>* writerVectors,
    std::vector<std::vector<OplogEntry>>* derivedOps) noexcept {
    fillWriterVectors(opCtx, ops, writerVectors, derivedOps);
}

Status applyOplogEntryOrGroupedInserts(OperationContext* opCtx,
                                       const OplogEntryOrGroupedInserts& entryOrGroupedInserts,
                                       OplogApplication::Mode oplogApplicationMode,
                                       const bool isDataConsistent) {
    // Guarantees that applyOplogEntryOrGroupedInserts' context matches that of its calling
    // function, applyOplogBatchPerWorker.
    invariant(!opCtx->writesAreReplicated());

    // Count each log op application as a separate operation, for reporting purposes

    auto incrementOpsAppliedStats = [] { opsAppliedStats.increment(1); };

    auto clockSource = opCtx->getServiceContext()->getFastClockSource();
    auto applyStartTime = clockSource->now();

    if (MONGO_unlikely(hangAfterRecordingOpApplicationStartTime.shouldFail())) {
        LOGV2(21233,
              "applyOplogEntryOrGroupedInserts - fail point "
              "hangAfterRecordingOpApplicationStartTime "
              "enabled. Blocking until fail point is disabled");
        hangAfterRecordingOpApplicationStartTime.pauseWhileSet();
    }

    auto status = OplogApplierUtils::applyOplogEntryOrGroupedInsertsCommon(opCtx,
                                                                           entryOrGroupedInserts,
                                                                           oplogApplicationMode,
                                                                           isDataConsistent,
                                                                           incrementOpsAppliedStats,
                                                                           &replOpCounters);

    auto op = entryOrGroupedInserts.getOp();
    if (op.getOpType() == OpTypeEnum::kNoop) {
        // No-ops should never fail application, since there's nothing to do.
        invariant(status.isOK());

        auto opObj = op.getObject();
        if (opObj.hasField(ReplicationCoordinator::newPrimaryMsgField) &&
            opObj.getField(ReplicationCoordinator::newPrimaryMsgField).str() ==
                ReplicationCoordinator::newPrimaryMsg) {

            ReplicationMetrics::get(opCtx).setParticipantNewTermDates(op.getWallClockTime(),
                                                                      applyStartTime);
        }

        return status;
    } else {
        return finishAndLogApply(opCtx, clockSource, status, applyStartTime, entryOrGroupedInserts);
    }
}

Status OplogApplierImpl::applyOplogBatchPerWorker(OperationContext* opCtx,
                                                  std::vector<const OplogEntry*>* ops,
                                                  WorkerMultikeyPathInfo* workerMultikeyPathInfo,
                                                  const bool isDataConsistent) {
    UnreplicatedWritesBlock uwb(opCtx);
    // Since we swap the locker in stash / unstash transaction resources,
    // ShouldNotConflictWithSecondaryBatchApplicationBlock will touch the locker that has been
    // destroyed by unstash in its destructor. Thus we set the flag explicitly.
    opCtx->lockState()->setShouldConflictWithSecondaryBatchApplication(false);

    // Ensure future transactions read without a timestamp.
    invariant(RecoveryUnit::ReadSource::kNoTimestamp ==
              opCtx->recoveryUnit()->getTimestampReadSource());

    // When querying indexes, we return the record matching the key if it exists, or an adjacent
    // document. This means that it is possible for us to hit a prepare conflict if we query for an
    // incomplete key and an adjacent key is prepared.
    // We ignore prepare conflicts on secondaries because they may encounter prepare conflicts that
    // did not occur on the primary.
    opCtx->recoveryUnit()->setPrepareConflictBehavior(
        PrepareConflictBehavior::kIgnoreConflictsAllowWrites);

    {  // Ensure that the MultikeyPathTracker stops tracking paths.
        ON_BLOCK_EXIT([opCtx] { MultikeyPathTracker::get(opCtx).stopTrackingMultikeyPathInfo(); });
        MultikeyPathTracker::get(opCtx).startTrackingMultikeyPathInfo();
        auto status = OplogApplierUtils::applyOplogBatchCommon(
            opCtx,
            ops,
            getOptions().mode,
            getOptions().allowNamespaceNotFoundErrorsOnCrudOps,
            isDataConsistent,
            &applyOplogEntryOrGroupedInserts);
        if (!status.isOK())
            return status;
    }

    invariant(!MultikeyPathTracker::get(opCtx).isTrackingMultikeyPathInfo());
    invariant(workerMultikeyPathInfo->empty());
    auto newPaths = MultikeyPathTracker::get(opCtx).getMultikeyPathInfo();
    if (!newPaths.empty()) {
        workerMultikeyPathInfo->swap(newPaths);
    }

    return Status::OK();
}

}  // namespace repl
}  // namespace mongo
