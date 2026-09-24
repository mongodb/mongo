// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/pipelined_applier_batch_tracker.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"


namespace mongo::repl {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforePipelinedApplierBatchWait);
MONGO_FAIL_POINT_DEFINE(hangBeforePipelinedApplierIdleWait);

}  // namespace

std::shared_ptr<std::atomic<uint32_t>> PipelinedApplierBatchTracker::addBatch(
    OpTimeAndWallTime lastOpTime, uint32_t numWorkers) {
    auto remainingWorkers = std::make_shared<std::atomic<uint32_t>>(numWorkers);
    {
        std::lock_guard lk(_mutex);
        invariant(!_shutdown);
        _inflightBatches.push_back({lastOpTime, remainingWorkers});
    }
    // It's possible to enqueue a batch with zero remaining workers, with the intention of advancing
    // lastApplied immediately.
    _cv.notify_one();
    return remainingWorkers;
}

void PipelinedApplierBatchTracker::onWorkerCompletion(
    const std::shared_ptr<std::atomic<uint32_t>>& remainingWorkers) {
    invariant(remainingWorkers);
    // A zero count marks the batch complete.
    auto previous = remainingWorkers->fetch_sub(1, std::memory_order_acq_rel);
    invariant(previous > 0);
    if (previous == 1) {
        // popCompletedBatch checks the front batch's counter while holding _mutex.
        // Taking the same mutex prevents notifying after it sees a nonzero count but before
        // it starts waiting on _cv.
        std::lock_guard lk(_mutex);
        _cv.notify_one();
    }
}

void PipelinedApplierBatchTracker::onWorkerAbandonment() {
    {
        std::lock_guard lk(_mutex);
        _hasAbandonedBatch = true;
    }
    _idleCv.notify_all();
}

boost::optional<PipelinedApplierBatchTracker::InflightBatch>
PipelinedApplierBatchTracker::popCompletedBatch(Milliseconds maxWaitTime) {
    std::unique_lock lk(_mutex);
    invariant(!_advancerBusy);
    auto batchIsComplete = [&] {
        if (_inflightBatches.empty() ||
            _inflightBatches.front().remainingWorkers->load(std::memory_order_acquire) != 0) {
            // Pause after a false predicate check, with _mutex still held.
            hangBeforePipelinedApplierBatchWait.pauseWhileSet();
            return false;
        }
        return true;
    };
    if (!_cv.wait_for(
            lk, maxWaitTime.toSystemDuration(), [&] { return _shutdown || batchIsComplete(); }) ||
        _shutdown) {
        // Return an empty optional on shutdown or a timeout with an empty queue or incomplete front
        // batch.
        return boost::none;
    }
    auto batch = std::move(_inflightBatches.front());
    _inflightBatches.pop_front();
    // Popping and setting the busy flag are atomic with respect to a concurrent idle wait, as
    // waitUntilIdle takes the mutex.
    _advancerBusy = true;
    return batch;
}

void PipelinedApplierBatchTracker::onBatchPublished() {
    std::lock_guard lk(_mutex);
    invariant(_advancerBusy);
    _advancerBusy = false;
    if (_inflightBatches.empty()) {
        _idleCv.notify_all();
    }
}

bool PipelinedApplierBatchTracker::waitUntilIdle() {
    std::unique_lock lk(_mutex);
    _idleCv.wait(lk, [&] {
        if (_hasAbandonedBatch || (_inflightBatches.empty() && !_advancerBusy)) {
            return true;
        }
        hangBeforePipelinedApplierIdleWait.pauseWhileSet();
        return false;
    });
    return !_hasAbandonedBatch;
}

void PipelinedApplierBatchTracker::shutdown() {
    {
        std::lock_guard lk(_mutex);
        _shutdown = true;
    }
    _cv.notify_one();
}

bool PipelinedApplierBatchTracker::isShutdown() const {
    std::lock_guard lk(_mutex);
    return _shutdown;
}

}  // namespace mongo::repl
