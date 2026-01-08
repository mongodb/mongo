/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/cancelable_operation_context.h"
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
        CancelableOperationContextFactory factory) = 0;

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
        CancelableOperationContextFactory factory) override;

    void dispose(OperationContext* opCtx) override;

private:
    std::vector<repl::OplogEntry> _fillBatch();

    ExecutorFuture<std::vector<repl::OplogEntry>> _getNextBatch(
        std::shared_ptr<executor::TaskExecutor> executor,
        CancellationToken cancelToken,
        CancelableOperationContextFactory factory);

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
