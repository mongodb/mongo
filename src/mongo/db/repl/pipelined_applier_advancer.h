// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/pipelined_applier_batch_tracker.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/modules.h"

#include <functional>

namespace mongo::repl {

/**
 * Publishes fully applied oplog batches on a dedicated thread in FIFO order. The publication
 * callback runs outside the tracker's mutex while the tracker remains busy. Lifecycle calls are
 * serialized by the caller, which starts the workers first and joins them after the advancer.
 */
class [[MONGO_MOD_PUBLIC]] PipelinedApplierAdvancer {
public:
    using PublishBatchFn = std::function<void(const PipelinedApplierBatchTracker::InflightBatch&)>;

    PipelinedApplierAdvancer(PipelinedApplierBatchTracker& batchTracker,
                             PublishBatchFn publishBatch);
    ~PipelinedApplierAdvancer();

    void startup();

    // Publishes dispatched work unless interrupted at global shutdown.
    void shutdownAndJoin();

private:
    // Publishes each completed front batch.
    void _advancerLoop();

    PipelinedApplierBatchTracker& _batchTracker;
    const PublishBatchFn _publishBatch;
    stdx::thread _thread;
};

}  // namespace mongo::repl
