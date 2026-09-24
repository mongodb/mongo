// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/optime.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include <boost/optional.hpp>

namespace mongo::repl {

/**
 * Tracks dispatched oplog batches in FIFO order with a shared remaining-workers counter per batch,
 * for use with the pipelined oplog applier. A single dispatcher registers batches in the tracker
 * before scheduling its work, workers report completion, and a single consumer removes completed
 * batches without skipping over an in-flight batch. A popped batch keeps the tracker busy until
 * publication of that batch finishes, so an idle tracker has no unpublished work.
 */
class [[MONGO_MOD_PUBLIC]] PipelinedApplierBatchTracker {
public:
    struct InflightBatch {
        OpTimeAndWallTime lastOpTime;
        std::shared_ptr<std::atomic<uint32_t>> remainingWorkers;
    };

    // Registers a batch before dispatch and returns the counter shared by its worker slices.
    std::shared_ptr<std::atomic<uint32_t>> addBatch(OpTimeAndWallTime lastOpTime,
                                                    uint32_t numWorkers);

    // Signals completion of one worker slice and notifies the consumer when its batch is complete.
    // Called from the worker thread.
    void onWorkerCompletion(const std::shared_ptr<std::atomic<uint32_t>>& remainingWorkers);

    // Records that a worker abandoned its slice at global shutdown.
    void onWorkerAbandonment();

    // Checks for a completed front batch for up to maxWaitTime, pops it if ready, otherwise returns
    // boost::none. Sets the tracker state to busy.
    boost::optional<InflightBatch> popCompletedBatch(Milliseconds maxWaitTime);

    // Finishes publication of the popped batch, including all of its side effects. Sets the tracker
    // state to idle.
    void onBatchPublished();

    // Waits for publication with dispatch paused, returning false if a worker abandoned a batch due
    // to shutdown.
    [[nodiscard]] bool waitUntilIdle();

    // Sets the shutdown flag and signals the consumer condition variable to return boost::none in
    // popCompletedBatch().
    void shutdown();

    // Returns whether consumer shutdown has been requested.
    bool isShutdown() const;

private:
    mutable std::mutex _mutex;
    stdx::condition_variable _cv;
    stdx::condition_variable _idleCv;
    std::deque<InflightBatch> _inflightBatches;  // (M)
    bool _advancerBusy = false;                  // (M)
    bool _hasAbandonedBatch = false;             // (M)
    bool _shutdown = false;                      // (M)
};

}  // namespace mongo::repl
