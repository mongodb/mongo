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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"
#include "mongo/util/log.h"

#include "mongo/db/repl/oplog_applier_impl.h"

namespace mongo {
namespace repl {


namespace {

class ApplyBatchFinalizer {
public:
    ApplyBatchFinalizer(ReplicationCoordinator* replCoord) : _replCoord(replCoord) {}
    virtual ~ApplyBatchFinalizer(){};

    virtual void record(const OpTimeAndWallTime& newOpTimeAndWallTime,
                        ReplicationCoordinator::DataConsistency consistency) {
        _recordApplied(newOpTimeAndWallTime, consistency);
    };

protected:
    void _recordApplied(const OpTimeAndWallTime& newOpTimeAndWallTime,
                        ReplicationCoordinator::DataConsistency consistency) {
        // We have to use setMyLastAppliedOpTimeAndWallTimeForward since this thread races with
        // ReplicationExternalStateImpl::onTransitionToPrimary.
        _replCoord->setMyLastAppliedOpTimeAndWallTimeForward(newOpTimeAndWallTime, consistency);
    }

    void _recordDurable(const OpTimeAndWallTime& newOpTimeAndWallTime) {
        // We have to use setMyLastDurableOpTimeAndWallTimeFoward since this thread races with
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

    void record(const OpTimeAndWallTime& newOpTimeAndWallTime,
                ReplicationCoordinator::DataConsistency consistency) override;

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

void ApplyBatchFinalizerForJournal::record(const OpTimeAndWallTime& newOpTimeAndWallTime,
                                           ReplicationCoordinator::DataConsistency consistency) {
    _recordApplied(newOpTimeAndWallTime, consistency);

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
        opCtx->recoveryUnit()->waitUntilDurable(opCtx.get());
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
      _storageInterface(storageInterface),
      _consistencyMarkers(consistencyMarkers),
      _syncTail(
          observer, consistencyMarkers, storageInterface, multiSyncApply, writerPool, options),
      _beginApplyingOpTime(options.beginApplyingOpTime) {}

void OplogApplierImpl::_run(OplogBuffer* oplogBuffer) {
    auto getNextApplierBatchFn = [this](OperationContext* opCtx, const BatchLimits& batchLimits) {
        return getNextApplierBatch(opCtx, batchLimits);
    };
    _runLoop(oplogBuffer, getNextApplierBatchFn);
}

void OplogApplierImpl::_shutdown() {
    _syncTail.shutdown();
}

void OplogApplierImpl::_runLoop(OplogBuffer* oplogBuffer,
                                OplogApplier::GetNextApplierBatchFn getNextApplierBatchFn) {
    // We don't start data replication for arbiters at all and it's not allowed to reconfig
    // arbiterOnly field for any member.
    invariant(!_replCoord->getMemberState().arbiter());

    OpQueueBatcher batcher(&_syncTail, _storageInterface, oplogBuffer, getNextApplierBatchFn);

    std::unique_ptr<ApplyBatchFinalizer> finalizer{
        getGlobalServiceContext()->getStorageEngine()->isDurable()
            ? new ApplyBatchFinalizerForJournal(_replCoord)
            : new ApplyBatchFinalizer(_replCoord)};

    while (true) {  // Exits on message from OpQueueBatcher.
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
            log() << "Oplog Applier - rsSyncApplyStop fail point enabled. Blocking until fail "
                     "point is disabled.";
            while (MONGO_unlikely(rsSyncApplyStop.shouldFail())) {
                // Tests should not trigger clean shutdown while that failpoint is active. If we
                // think we need this, we need to think hard about what the behavior should be.
                if (_syncTail.inShutdown()) {
                    severe() << "Turn off rsSyncApplyStop before attempting clean shutdown";
                    fassertFailedNoTrace(40304);
                }
                sleepmillis(10);
            }
        }

        // Transition to SECONDARY state, if possible.
        _replCoord->finishRecoveryIfEligible(&opCtx);

        // Blocks up to a second waiting for a batch to be ready to apply. If one doesn't become
        // ready in time, we'll loop again so we can do the above checks periodically.
        OpQueue ops = batcher.getNextBatch(Seconds(1));
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

        // Apply the operations in this batch. 'multiApply' returns the optime of the last op that
        // was applied, which should be the last optime in the batch.
        auto lastOpTimeAppliedInBatch =
            fassertNoTrace(34437, _syncTail.multiApply(&opCtx, ops.releaseBatch()));
        invariant(lastOpTimeAppliedInBatch == lastOpTimeInBatch);

        // Update various things that care about our last applied optime. Tests rely on 1 happening
        // before 2 even though it isn't strictly necessary.

        // 1. Persist our "applied through" optime to disk.
        _consistencyMarkers->setAppliedThrough(&opCtx, lastOpTimeInBatch);

        // 2. Ensure that the last applied op time hasn't changed since the start of this batch.
        const auto lastAppliedOpTimeAtEndOfBatch = _replCoord->getMyLastAppliedOpTime();
        invariant(lastAppliedOpTimeAtStartOfBatch == lastAppliedOpTimeAtEndOfBatch,
                  str::stream() << "the last known applied OpTime has changed from "
                                << lastAppliedOpTimeAtStartOfBatch.toString() << " to "
                                << lastAppliedOpTimeAtEndOfBatch.toString()
                                << " in the middle of batch application");

        // 3. Update oplog visibility by notifying the storage engine of the new oplog entries.
        const bool orderedCommit = true;
        _storageInterface->oplogDiskLocRegister(
            &opCtx, lastOpTimeInBatch.getTimestamp(), orderedCommit);

        // 4. Finalize this batch. We are at a consistent optime if our current optime is >= the
        // current 'minValid' optime. Note that recording the lastOpTime in the finalizer includes
        // advancing the global timestamp to at least its timestamp.
        const auto minValid = _consistencyMarkers->getMinValid(&opCtx);
        auto consistency = (lastOpTimeInBatch >= minValid)
            ? ReplicationCoordinator::DataConsistency::Consistent
            : ReplicationCoordinator::DataConsistency::Inconsistent;

        // The finalizer advances the global timestamp to lastOpTimeInBatch.
        finalizer->record({lastOpTimeInBatch, lastWallTimeInBatch}, consistency);
    }
}

StatusWith<OpTime> OplogApplierImpl::_multiApply(OperationContext* opCtx, Operations ops) {
    return _syncTail.multiApply(opCtx, std::move(ops));
}

}  // namespace repl
}  // namespace mongo
