// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/pipelined_applier_worker_pool.h"
#include "mongo/db/repl/pipelined_op_router.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/util/modules.h"

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

    ReplicationCoordinator* const _replCoord;
    StorageInterface* const _storageInterface;

    // Persistent worker threads that consume dispatched work items.
    PipelinedApplierWorkerPool _workerPool;

    // Classifies each oplog entry as pipelined or requiring inline
    // application, and selects the worker for pipelined entries by hash so ops on one document
    // are applied in order.
    PipelinedOpRouter _router;
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
                     const PipelinedApplierWorkerPool::WorkItem& item);

}  // namespace mongo::repl
