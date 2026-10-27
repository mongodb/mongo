// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/pipelined_applier_advancer.h"
#include "mongo/db/repl/pipelined_applier_batch_tracker.h"
#include "mongo/db/repl/pipelined_applier_worker_pool.h"
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
     * Performs the "drain, dispatch, drain" sequence for batches that must be applied serialized,
     * otherwise dispatches the batch to the worker pool and returns immediately.
     * Expects that the batch has been processed by the OplogApplierBatcher
     * (i.e. DDL operations sit alone, etc).
     */
    void _dispatchBatch(OperationContext* opCtx, std::vector<OplogEntry> ops);

    /**
     * Derives and routes one batch of oplog entries onto the workers and registers the batch with
     * the batch tracker.
     */
    void _dispatchBatchToWorkers(OperationContext* opCtx, std::vector<OplogEntry> ops);

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

    bool _disableTransactionUpdateCoalescing = false;

    // Tracks dispatched batches in FIFO order and signals when their workers finish.
    PipelinedApplierBatchTracker _batchTracker;

    // Persistent worker threads that consume dispatched work items.
    PipelinedApplierWorkerPool _workerPool;

    // Declared last so the advancer shuts down before the workers and batch tracker are destroyed.
    PipelinedApplierAdvancer _advancer;
};

/**
 * Applies the slice of ops in 'item', optionally capturing deferred multikey writes.
 */
[[nodiscard]] Status applyWorkItem(OperationContext* opCtx,
                                   const OplogApplier::Options& options,
                                   const PipelinedApplierWorkerPool::WorkItem& item,
                                   WorkerMultikeyPathInfo* multikeyPathInfo = nullptr);

/**
 * Applies 'item' on the current worker thread, fasserting on any failure other than shutdown.
 * Records multikey changes from the work item.
 */
// TODO(SERVER-135372): Remove deferMultikeyWrites and always defer once the advancer
// flushes multikey writes before publishing lastApplied.
void consumeWorkItem(size_t workerIdx,
                     const OplogApplier::Options& options,
                     const PipelinedApplierWorkerPool::WorkItem& item,
                     PipelinedApplierBatchTracker& batchTracker,
                     bool deferMultikeyWrites = false);

/**
 * Returns whether a batch must be applied serialized ("drain, dispatch, drain") because it
 * contains an entry that must not be applied concurrently with its neighbors.
 */
bool batchMustBeAppliedSerialized(OplogApplication::Mode mode, const std::vector<OplogEntry>& ops);

}  // namespace mongo::repl
