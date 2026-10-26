// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/pipelined_applier_batch_tracker.h"

#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo::repl {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforePipelinedApplierBatchWait);
MONGO_FAIL_POINT_DEFINE(hangBeforePipelinedApplierIdleWait);
MONGO_FAIL_POINT_DEFINE(hangBeforePipelinedApplierCapacityWait);

}  // namespace

std::shared_ptr<std::atomic<uint32_t>> PipelinedApplierBatchTracker::addBatch(
    OpTimeAndWallTime lastOpTime,
    uint32_t numWorkers,
    std::vector<OplogEntry> ops,
    std::vector<std::vector<OplogEntry>> derivedOps,
    std::shared_ptr<std::vector<WorkerMultikeyPathInfo>> multikeyMetadata,
    uint64_t batchBytes) {
    auto remainingWorkers = std::make_shared<std::atomic<uint32_t>>(numWorkers);
    {
        std::unique_lock lk(_mutex);
        invariant(!_shutdown);

        // Block until admission is allowed under both caps, or shutdown.
        auto maxBatches = pipelinedApplierMaxInflightBatches.loadRelaxed();
        auto maxBytes = pipelinedApplierMaxInflightBytes.loadRelaxed();
        double hysteresisFactor = pipelinedApplierHysteresisFactor.loadRelaxed();

        if (backpressureNecessary(_inflightBatches.size(), _inflightBytes, maxBatches, maxBytes)) {
            if (MONGO_unlikely(hangBeforePipelinedApplierCapacityWait.shouldFail())) {
                hangBeforePipelinedApplierCapacityWait.pauseWhileSet();
            }
            auto newBatchThreshold = static_cast<uint64_t>(maxBatches * hysteresisFactor);
            auto newBytesThreshold = static_cast<uint64_t>(maxBytes * hysteresisFactor);
            auto updateThresholds = [&] {
                maxBatches = pipelinedApplierMaxInflightBatches.loadRelaxed();
                maxBytes = pipelinedApplierMaxInflightBytes.loadRelaxed();
                hysteresisFactor = pipelinedApplierHysteresisFactor.loadRelaxed();
                newBatchThreshold = static_cast<uint64_t>(maxBatches * hysteresisFactor);
                newBytesThreshold = static_cast<uint64_t>(maxBytes * hysteresisFactor);
            };
            const auto underThresholdOrShuttingDown = [&] {
                updateThresholds();
                return _shutdown ||
                    !backpressureNecessary(_inflightBatches.size(),
                                           _inflightBytes,
                                           newBatchThreshold,
                                           newBytesThreshold);
            };
            // Emit a log message after having waited for 1sec, 30sec, 1hr, and 24hrs.
            const std::array logFrequencies{
                Seconds(1).toSystemDuration(),
                Seconds(30).toSystemDuration() - Seconds(1).toSystemDuration(),
                Hours(1).toSystemDuration() - Seconds(30).toSystemDuration(),
                Hours(24).toSystemDuration() - Hours(1).toSystemDuration(),
            };
            const auto start = Date_t::now();
            for (const auto& duration : logFrequencies) {
                if (_capacityCv.wait_for(lk, duration, underThresholdOrShuttingDown)) {
                    break;
                }
                LOGV2(13478000,
                      "Pipelined oplog applier backpressure triggered, waiting for the inflight "
                      "batch queue count/bytes to drop under threshold before enqueuing further",
                      "batchesOverThreshold"_attr = _inflightBatches.size() -
                          std::min(_inflightBatches.size(), newBatchThreshold),
                      "bytesOverThreshold"_attr =
                          _inflightBytes - std::min(_inflightBytes, newBytesThreshold),
                      "batchThreshold"_attr = newBatchThreshold,
                      "byteThreshold"_attr = newBytesThreshold,
                      "timeBlocked"_attr = Date_t::now() - start);
                // If this is the last duration in the loop, perform an unbounded wait.
                if (duration == logFrequencies.back()) {
                    _capacityCv.wait(lk, underThresholdOrShuttingDown);
                }
            }
            if (_shutdown) {
                return nullptr;
            }
        }
        _inflightBatches.push_back({lastOpTime,
                                    remainingWorkers,
                                    std::move(ops),
                                    std::move(derivedOps),
                                    batchBytes,
                                    std::move(multikeyMetadata)});
        _inflightBytes += batchBytes;
    }
    // It's possible to enqueue a batch with zero remaining workers, with the intention of advancing
    // lastApplied immediately.
    _cv.notify_one();
    return remainingWorkers;
}

boost::optional<PipelinedApplierBatchTracker::BackpressureReason>
PipelinedApplierBatchTracker::backpressureNecessary(uint64_t batchCount,
                                                    uint64_t byteCount,
                                                    uint64_t batchLimit,
                                                    uint64_t byteLimit) {
    auto batchesOverThreshold = batchCount - std::min(batchCount, batchLimit);
    auto bytesOverThreshold = byteCount - std::min(byteCount, byteLimit);
    if (!batchesOverThreshold && !bytesOverThreshold) {
        return {};
    }
    return {
        {.batchesOverThreshold = batchesOverThreshold, .bytesOverThreshold = bytesOverThreshold}};
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
    invariant(_inflightBytes >= batch.batchBytes);
    _inflightBytes -= batch.batchBytes;
    // Popping and setting the busy flag are atomic with respect to a concurrent idle wait, as
    // waitUntilIdle takes the mutex.
    _advancerBusy = true;
    _capacityCv.notify_one();
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
    _capacityCv.notify_all();
}

bool PipelinedApplierBatchTracker::isShutdown() const {
    std::lock_guard lk(_mutex);
    return _shutdown;
}

}  // namespace mongo::repl


