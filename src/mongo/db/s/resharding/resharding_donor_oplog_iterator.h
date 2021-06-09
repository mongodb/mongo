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

#include <vector>

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/future.h"

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
 * Instances of this class are not thread-safe.
 */
class ReshardingDonorOplogIterator : public ReshardingDonorOplogIteratorInterface {
public:
    ReshardingDonorOplogIterator(NamespaceString oplogBufferNss,
                                 ReshardingDonorOplogId resumeToken,
                                 resharding::OnInsertAwaitable* insertNotifier);

    /**
     * Returns a pipeline for iterating the buffered copy of the donor's oplog.
     *
     * The documents returned by the pipeline have the oplog entries linked together with their
     * preImage/postImage entries.
     */
    std::unique_ptr<Pipeline, PipelineDeleter> makePipeline(
        OperationContext* opCtx, std::shared_ptr<MongoProcessInterface> mongoProcessInterface);

    ExecutorFuture<std::vector<repl::OplogEntry>> getNextBatch(
        std::shared_ptr<executor::TaskExecutor> executor,
        CancellationToken cancelToken,
        CancelableOperationContextFactory factory) override;

    void dispose(OperationContext* opCtx) override;

    static constexpr auto kActualOpFieldName = "actualOp"_sd;
    static constexpr auto kPreImageOpFieldName = "preImageOp"_sd;
    static constexpr auto kPostImageOpFieldName = "postImageOp"_sd;

private:
    std::vector<repl::OplogEntry> _fillBatch(Pipeline& pipeline);

    const NamespaceString _oplogBufferNss;

    ReshardingDonorOplogId _resumeToken;

    // _insertNotifier is used to asynchronously wait for a document to be inserted into the oplog
    // buffer collection by the ReshardingOplogFetcher after _pipeline is exhausted and the final
    // oplog entry hasn't been reached yet.
    resharding::OnInsertAwaitable* const _insertNotifier;

    std::unique_ptr<Pipeline, PipelineDeleter> _pipeline;
    bool _hasSeenFinalOplogEntry{false};
};

}  // namespace mongo
