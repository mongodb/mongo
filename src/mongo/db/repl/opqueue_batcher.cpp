/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/repl/opqueue_batcher.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {


OpQueueBatcher::OpQueueBatcher(SyncTail* syncTail,
                               StorageInterface* storageInterface,
                               OplogBuffer* oplogBuffer,
                               OplogApplier::GetNextApplierBatchFn getNextApplierBatchFn)
    : _syncTail(syncTail),
      _storageInterface(storageInterface),
      _oplogBuffer(oplogBuffer),
      _getNextApplierBatchFn(getNextApplierBatchFn),
      _ops(0),
      _thread([this] { run(); }) {}
OpQueueBatcher::~OpQueueBatcher() {
    invariant(_isDead);
    _thread.join();
}


OpQueue OpQueueBatcher::getNextBatch(Seconds maxWaitTime) {
    stdx::unique_lock<Latch> lk(_mutex);
    // _ops can indicate the following cases:
    // 1. A new batch is ready to consume.
    // 2. Shutdown.
    // 3. The batch has (or had) exhausted the buffer in draining mode.
    // 4. Empty batch since the batch has/had exhausted the buffer but not in draining mode,
    //    so there could be new oplog entries coming.
    // 5. Empty batch since the batcher is still running.
    //
    // In case (4) and (5), we wait for up to "maxWaitTime".
    if (_ops.empty() && !_ops.mustShutdown() && !_ops.termWhenExhausted()) {
        // We intentionally don't care about whether this returns due to signaling or timeout
        // since we do the same thing either way: return whatever is in _ops.
        (void)_cv.wait_for(lk, maxWaitTime.toSystemDuration());
    }

    OpQueue ops = std::move(_ops);
    _ops = OpQueue(0);
    _cv.notify_all();
    return ops;
}

/**
 * If slaveDelay is enabled, this function calculates the most recent timestamp of any oplog
 * entries that can be be returned in a batch.
 */
boost::optional<Date_t> OpQueueBatcher::_calculateSlaveDelayLatestTimestamp() {
    auto service = cc().getServiceContext();
    auto replCoord = ReplicationCoordinator::get(service);
    auto slaveDelay = replCoord->getSlaveDelaySecs();
    if (slaveDelay <= Seconds(0)) {
        return {};
    }
    auto fastClockSource = service->getFastClockSource();
    return fastClockSource->now() - slaveDelay;
}

void OpQueueBatcher::run() {
    Client::initThread("ReplBatcher");

    OplogApplier::BatchLimits batchLimits;

    while (true) {
        rsSyncApplyStop.pauseWhileSet();

        batchLimits.slaveDelayLatestTimestamp = _calculateSlaveDelayLatestTimestamp();

        // Check the limits once per batch since users can change them at runtime.
        batchLimits.ops = getBatchLimitOplogEntries();

        OpQueue ops(batchLimits.ops);
        {
            auto opCtx = cc().makeOperationContext();

            // This use of UninterruptibleLockGuard is intentional. It is undesirable to use an
            // UninterruptibleLockGuard in client operations because stepdown requires the
            // ability to interrupt client operations. However, it is acceptable to use an
            // UninterruptibleLockGuard in batch application because the only cause of
            // interruption would be shutdown, and the ReplBatcher thread has its own shutdown
            // handling.
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());

            // Locks the oplog to check its max size, do this in the UninterruptibleLockGuard.
            batchLimits.bytes = getBatchLimitOplogBytes(opCtx.get(), _storageInterface);

            auto oplogEntries =
                fassertNoTrace(31004, _getNextApplierBatchFn(opCtx.get(), batchLimits));
            for (const auto& oplogEntry : oplogEntries) {
                ops.emplace_back(oplogEntry);
            }

            // If we don't have anything in the queue, wait a bit for something to appear.
            if (oplogEntries.empty()) {
                if (_syncTail->inShutdown()) {
                    ops.setMustShutdownFlag();
                } else {
                    // Block up to 1 second.
                    _oplogBuffer->waitForData(Seconds(1));
                }
            }
        }

        if (ops.empty() && !ops.mustShutdown()) {
            // Check whether we have drained the oplog buffer. The states checked here can be
            // stale when it's used by the applier. signalDrainComplete() needs to check the
            // applier is still draining in the same term to make sure these states have not
            // changed.
            auto replCoord = ReplicationCoordinator::get(cc().getServiceContext());
            // Check the term first to detect DRAINING -> RUNNING -> DRAINING when signaling
            // drain complete.
            //
            // Batcher can delay arbitrarily. After stepup, if the batcher drained the buffer
            // and blocks when it's about to notify the applier to signal drain complete, the
            // node may step down and fetch new data into the buffer and then step up again.
            // Now the batcher will resume and let the applier signal drain complete even if
            // the buffer has new data. Checking the term before and after ensures nothing
            // changed in between.
            auto termWhenBufferIsEmpty = replCoord->getTerm();
            // Draining state guarantees the producer has already been fully stopped and no more
            // operations will be pushed in to the oplog buffer until the applier state changes.
            auto isDraining =
                replCoord->getApplierState() == ReplicationCoordinator::ApplierState::Draining;
            // Check the oplog buffer after the applier state to ensure the producer is stopped.
            if (isDraining && _oplogBuffer->isEmpty()) {
                ops.setTermWhenExhausted(termWhenBufferIsEmpty);
                log() << "Oplog buffer has been drained in term " << termWhenBufferIsEmpty;
            } else {
                // Don't emit empty batches.
                continue;
            }
        }

        stdx::unique_lock<Latch> lk(_mutex);
        // Block until the previous batch has been taken.
        _cv.wait(lk, [&] { return _ops.empty() && !_ops.termWhenExhausted(); });
        _ops = std::move(ops);
        _cv.notify_all();
        if (_ops.mustShutdown()) {
            _isDead = true;
            return;
        }
    }
}

}  // namespace repl
}  // namespace mongo
