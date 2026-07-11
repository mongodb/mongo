// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_pipeline.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {

class OperationContext;

namespace resharding {

class OnInsertAwaitable {
public:
    virtual ~OnInsertAwaitable() = default;

    /**
     * Returns a future that becomes ready when the {_id: lastSeen} document is no longer the last
     * inserted document in the oplog buffer collection.
     */
    virtual Future<void> awaitInsert(const ReshardingDonorOplogId& lastSeen) = 0;
};

}  // namespace resharding

class ReshardingDonorOplogIteratorInterface {
public:
    virtual ~ReshardingDonorOplogIteratorInterface() = default;

    /**
     * Returns the next batch of oplog entries to apply.
     *
     *  - An empty vector is returned when there are no more oplog entries left to apply.
     *  - A non-immediately ready future is returned when the iterator has been exhausted, but the
     *    final oplog entry hasn't been returned yet.
     */
    virtual ExecutorFuture<std::vector<repl::OplogEntry>> getNextBatch(
        std::shared_ptr<executor::TaskExecutor> executor,
        CancellationToken cancelToken,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) = 0;

    /**
     * Releases any resources held by this oplog iterator such as Pipelines, PlanExecutors, or
     * in-memory structures.
     *
     * This function must be called before destroying the oplog iterator.
     */
    virtual void dispose(OperationContext* opCtx) {}
};

/**
 * Iterator for fetching batches of oplog entries from the oplog buffer collection for a particular
 * donor shard.
 *
 * Supports auto-retry on retriable errors and waiting for new oplog entries to be inserted
 * to the buffer collection.
 *
 * Instances of this class are not thread-safe.
 */
class ReshardingDonorOplogIterator : public ReshardingDonorOplogIteratorInterface {
public:
    ReshardingDonorOplogIterator(std::unique_ptr<ReshardingDonorOplogPipelineInterface> pipeline,
                                 ReshardingDonorOplogId resumeToken,
                                 resharding::OnInsertAwaitable* insertNotifier);

    ExecutorFuture<std::vector<repl::OplogEntry>> getNextBatch(
        std::shared_ptr<executor::TaskExecutor> executor,
        CancellationToken cancelToken,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) override;

    void dispose(OperationContext* opCtx) override;

private:
    std::vector<repl::OplogEntry> _fillBatch();

    ExecutorFuture<std::vector<repl::OplogEntry>> _getNextBatch(
        std::shared_ptr<executor::TaskExecutor> executor,
        CancellationToken cancelToken,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    const NamespaceString _oplogBufferNss;

    std::unique_ptr<ReshardingDonorOplogPipelineInterface> _pipeline;
    ReshardingDonorOplogId _resumeToken;

    // _insertNotifier is used to asynchronously wait for a document to be inserted into the oplog
    // buffer collection by the ReshardingOplogFetcher after _pipeline is exhausted and the final
    // oplog entry hasn't been reached yet.
    resharding::OnInsertAwaitable* const _insertNotifier;

    bool _hasSeenFinalOplogEntry{false};
};

}  // namespace mongo
