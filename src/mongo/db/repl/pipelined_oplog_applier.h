// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/pipelined_applier_advancer.h"
#include "mongo/db/repl/pipelined_applier_batch_tracker.h"
#include "mongo/db/repl/pipelined_applier_worker_pool.h"
#include "mongo/db/repl/pipelined_op_router.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo::repl {

/**
 * An oplog applier that applies batches concurrently rather than one at a time. A dispatcher
 * routes each batch's ops onto persistent worker threads with individual queues and immediately
 * proceeds to the next batch, while an advancer thread publishes lastApplied from a FIFO queue of
 * fully-applied batches.
 */
class [[MONGO_MOD_PUBLIC]] PipelinedOplogApplier final : public OplogApplier {
    PipelinedOplogApplier(const PipelinedOplogApplier&) = delete;
    PipelinedOplogApplier& operator=(const PipelinedOplogApplier&) = delete;

public:
    PipelinedOplogApplier(executor::TaskExecutor* executor,
                          OplogBuffer* oplogBuffer,
                          Observer* observer,
                          ReplicationCoordinator* replCoord,
                          StorageInterface* storageInterface,
                          const Options& options,
                          size_t numWorkers);

    ~PipelinedOplogApplier() override;

private:
    void _run(OplogBuffer* oplogBuffer) override;

    StatusWith<OpTime> _applyOplogBatch(OperationContext* opCtx,
                                        std::vector<OplogEntry> ops) override;

    /**
     * Classifies and hash-routes one batch onto the workers, enqueuing one work item per
     * participating worker per batch. Expands any single ops that may contain multiple ops to
     * different keys if needed (e.g. container writes or applyOps)
     */
    void _dispatchOps(OperationContext* opCtx, std::vector<OplogEntry> ops);

    // Waits for all queued and active worker tasks to finish. Returns true once all dispatched
    // batches are fully published, meaning worker threads and the advancer are idle. Returns false
    // if a worker abandoned work during shutdown; the pipeline is not fully drained.
    [[nodiscard]] bool _drainWorkers();

    // Waits for publication of all registered batches, including all publication side effects.
    // Returns true when the FIFO is empty and the advancer has finished publishing.
    // Returns false if a worker abandoned work during shutdown, without guaranteeing publication.
    // Requires dispatch to be paused.
    [[nodiscard]] bool _waitForAdvancerIdle();

    // Publishes a completed batch's replication progress and wakes oplog waiters.
    void _publishBatch(const PipelinedApplierBatchTracker::InflightBatch& batch);

    ReplicationCoordinator* const _replCoord;
    StorageInterface* const _storageInterface;

    // Tracks dispatched batches in FIFO order and signals when their workers finish.
    PipelinedApplierBatchTracker _batchTracker;

    // Persistent worker threads that consume dispatched work items.
    PipelinedApplierWorkerPool _workerPool;

    // Classifies each oplog entry as pipelined or requiring inline application, and selects the
    // worker for pipelined entries by hash so ops on one document are applied in order.
    PipelinedOpRouter _router;

    // Declared last so the advancer shuts down before the workers and batch tracker are destroyed.
    PipelinedApplierAdvancer _advancer;
};

/**
 * Applies the slice of ops in 'item' in order.
 */
[[nodiscard]] Status applyWorkItem(OperationContext* opCtx,
                                   const OplogApplier::Options& options,
                                   const PipelinedApplierWorkerPool::WorkItem& item);

/**
 * Applies 'item' on the current worker thread, fasserting on any failure other than shutdown.
 */
void consumeWorkItem(size_t workerIdx,
                     const OplogApplier::Options& options,
                     const PipelinedApplierWorkerPool::WorkItem& item,
                     PipelinedApplierBatchTracker& batchTracker);

}  // namespace mongo::repl
