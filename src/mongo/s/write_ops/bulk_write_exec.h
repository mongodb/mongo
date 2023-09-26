/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <boost/optional/optional.hpp>
#include <memory>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/commands/bulk_write_gen.h"
#include "mongo/db/commands/bulk_write_parser.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/ns_targeter.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/write_op.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {
namespace bulk_write_exec {

/**
 * Contains replies for individual bulk write ops along with a count of how many replies in the
 * vector are errors.
 */
struct BulkWriteReplyInfo {
    std::vector<BulkWriteReplyItem> replyItems;
    int numErrors = 0;
    boost::optional<BulkWriteWriteConcernError> wcErrors;
    boost::optional<std::vector<StmtId>> retriedStmtIds;
};

/**
 * Attempt to run the bulkWriteCommandRequest through Queryable Encryption code path.
 * Returns kNotProcessed if falling back to the regular bulk write code path is needed instead.
 *
 * This function does not throw, any errors are reported via the function return.
 */
std::pair<FLEBatchResult, BulkWriteReplyInfo> attemptExecuteFLE(
    OperationContext* opCtx, const BulkWriteCommandRequest& clientRequest);

/**
 * Executes a client bulkWrite request by sending child batches to several shard endpoints, and
 * returns a vector of BulkWriteReplyItem (each of which is a reply for an individual op) along
 * with a count of how many of those replies are errors.
 *
 * This function does not throw, any errors are reported via the function return.
 */
BulkWriteReplyInfo execute(OperationContext* opCtx,
                           const std::vector<std::unique_ptr<NSTargeter>>& targeters,
                           const BulkWriteCommandRequest& clientRequest);

/**
 * The BulkWriteOp class manages the lifecycle of a bulkWrite request received by mongos. Each op in
 * the ops array is tracked via a WriteOp, and the function of the BulkWriteOp is to aggregate the
 * dispatched requests and responses for the underlying WriteOps.
 *
 * Overall, the BulkWriteOp lifecycle is similar to the WriteOp lifecycle, with the following
 * stages:
 *
 * 0) Client request comes in, a BulkWriteOp is initialized.
 *
 * 1a) One or more ops in the bulkWrite are targeted, resulting in TargetedWriteBatches for these
 *     ops. OR
 * 1b) There are targeting errors, and the ops must be retargeted after refreshing the
 *     NSTargeter(s).
 *
 * 2) Child bulkWrite requests (referred to in code as child batches) are built for each
 *    TargetedWriteBatch before sending.
 *
 * 3) Responses for sent child batches are noted, and errors are stored and aggregated per-write-op.
 *    Certain errors are returned immediately (e.g. any error in a transaction).
 *
 * 4) If the whole bulkWrite is not finished, goto 0.
 *
 * 5) When all responses come back for all write ops, success responses and errors are aggregated
 *    and returned in a client response.
 *
 */
class BulkWriteOp {
    BulkWriteOp(const BulkWriteOp&) = delete;
    BulkWriteOp& operator=(const BulkWriteOp&) = delete;

public:
    BulkWriteOp() = delete;
    BulkWriteOp(OperationContext* opCtx, const BulkWriteCommandRequest& clientRequest);
    ~BulkWriteOp() = default;

    /**
     * Targets one or more of the next write ops in this bulkWrite request using the given
     * NSTargeters (targeters[i] corresponds to the targeter of the collection in nsInfo[i]). The
     * resulting TargetedWrites are aggregated together in the returned TargetedWriteBatches.
     *
     * If 'recordTargetErrors' is false, any targeting error will abort all current batches and
     * the method will return the targeting error. No targetedBatches will be returned on error.
     *
     * Otherwise, if 'recordTargetErrors' is true, targeting errors will be recorded for each
     * write op that fails to target, and the method will return OK.
     *
     * (The idea here is that if we are sure our NSTargeters are up-to-date we should record
     * targeting errors, but if not we should refresh once first.)
     *
     * Returned TargetedWriteBatches are owned by the caller.
     * If a write without a shard key or a time-series retryable update is detected, return an OK
     * StatusWith that has the corresponding WriteType as the value.
     */
    StatusWith<WriteType> target(const std::vector<std::unique_ptr<NSTargeter>>& targeters,
                                 bool recordTargetErrors,
                                 TargetedBatchMap& targetedBatches);

    /**
     * Fills a BulkWriteCommandRequest from a TargetedWriteBatch for this BulkWriteOp.
     */
    BulkWriteCommandRequest buildBulkCommandRequest(
        const std::vector<std::unique_ptr<NSTargeter>>& targeters,
        const TargetedWriteBatch& targetedBatch,
        boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery) const;

    /**
     * Returns false if the bulk write op needs more processing.
     */
    bool isFinished() const;

    const WriteOp& getWriteOp_forTest(int i) const;

    int numWriteOpsIn(WriteOpState opState) const;

    /**
     * Saves all the write concern errors received from all the shards so that they can
     * be concatenated into a single error when mongos responds to the client.
     */
    void saveWriteConcernError(ShardId shardId, BulkWriteWriteConcernError wcError);
    void saveWriteConcernError(ShardId shardId, WriteConcernErrorDetail wce);
    std::vector<ShardWCError> getWriteConcernErrors() const {
        return _wcErrors;
    }

    /**
     * Marks any further writes for this BulkWriteOp as failed with the provided error status. There
     * must be no pending ops awaiting results when this method is called.
     */
    void noteErrorForRemainingWrites(const Status& status);

    /*
     * Handles errors for the response from a retryable timeseries update child batch.
     */
    void handleErrorsForRetryableTimeseriesUpdate(
        StatusWith<mongo::txn_api::CommitResult>& swResult, const ShardId& shardId);

    /**
     * Processes the response to a TargetedWriteBatch. Sharding related errors are then grouped
     * by namespace and captured in the map passed in.
     */
    void noteChildBatchResponse(
        const TargetedWriteBatch& targetedBatch,
        const std::vector<BulkWriteReplyItem>& replyItems,
        const boost::optional<std::vector<StmtId>>& retriedStmtIds,
        boost::optional<stdx::unordered_map<NamespaceString, TrackedErrors>&> errorsPerNamespace);


    void processChildBatchResponseFromRemote(
        const TargetedWriteBatch& writeBatch,
        const AsyncRequestsSender::Response& response,
        boost::optional<stdx::unordered_map<NamespaceString, TrackedErrors>&> errorsPerNamespace);

    /**
     * Records the error contained in the given status for write(s) in the given targetedBatch.
     * This is used in cases where we get a top-level error in response to a batch sent to a shard
     * but that we do not want to return the top-level error directly to the user.
     * Instead, we treat the error as a failure of the relevant write(s) within the batch: for
     * unordered writes that is all writes in the batch, and for ordered writes it is only the first
     * write (since we would stop after that failed and not attempt execution of further writes.)
     */
    void noteChildBatchError(const TargetedWriteBatch& targetedBatch, const Status& status);

    /**
     * Processes a local error encountered while trying to send a child batch to a shard. This could
     * be e.g. a network error or an error due to this mongos shutting down.
     */
    void processLocalChildBatchError(const TargetedWriteBatch& batch,
                                     const AsyncRequestsSender::Response& response);

    /**
     * Processes the response to a single WriteOp at index opIdx directly and cleans up all
     * associated childOps. The response is captured by the BulkWriteReplyItem. We don't expect
     * sharding related stale version/db errors because response set by this method should be final
     * (i.e. not retryable).
     *
     * This is currently used by retryable timeseries updates and writes without shard key because
     * those operations are processed individually with the use of internal transactions.
     */
    void noteWriteOpFinalResponse(size_t opIdx,
                                  const BulkWriteReplyItem& reply,
                                  const boost::optional<std::vector<StmtId>>& retriedStmtIds);

    /**
     * Mark the corresponding targeter stale based on errorsPerNamespace.
     */
    void noteStaleResponses(
        const std::vector<std::unique_ptr<NSTargeter>>& targeters,
        const stdx::unordered_map<NamespaceString, TrackedErrors>& errorsPerNamespace);

    /**
     * Returns a vector of BulkWriteReplyItem based on the end state of each individual write in
     * this bulkWrite operation, along with the number of error replies contained in the vector.
     */
    BulkWriteReplyInfo generateReplyInfo();

    /**
     * Creates a BulkWriteWriteConcernError object which combines write concern errors
     * from all shards. If no write concern errors exist, returns boost::none.
     */
    boost::optional<BulkWriteWriteConcernError> generateWriteConcernError() const;

    /**
     * Calculates an estimate of the size, in bytes, required to store the common fields that will
     * go into each child batch command sent to a shard, i.e. all fields besides the actual write
     * ops.
     */
    int getBaseChildBatchCommandSizeEstimate() const;

    const BulkWriteCommandRequest& getClientRequest() const {
        return _clientRequest;
    }

private:
    // The OperationContext the client bulkWrite request is run on.
    OperationContext* const _opCtx;

    // The incoming client bulkWrite request.
    const BulkWriteCommandRequest& _clientRequest;

    // Array of ops being processed from the client bulkWrite request.
    std::vector<WriteOp> _writeOps;

    // Cached transaction number (if one is present on the operation contex).
    boost::optional<TxnNumber> _txnNum;

    // The write concern that the bulk write command was issued with.
    WriteConcernOptions _writeConcern;
    // A list of write concern errors from all shards.
    std::vector<ShardWCError> _wcErrors;

    // Statement ids for the ops that had already been executed, thus were not executed in this
    // bulkWrite.
    boost::optional<std::vector<StmtId>> _retriedStmtIds;

    // Set to true if this write is part of a transaction.
    const bool _inTransaction{false};
    const bool _isRetryableWrite{false};

    // Set to true if we encountered an error that prevents us from executing the rest of the
    // bulkWrite. Note this does *not* include cases where we saw an error for an individual
    // statement in an ordered bulkWrite, but instead covers these cases:
    // - Any error encountered while in a transaction.
    // - A local error indicating that this process is shutting down.
    bool _aborted = false;
};

/**
 * Adds an _id field to any document to insert that is missing one. It is necessary to add _id on
 * mongos so that, if _id is in the shard key pattern, we can correctly route the insert based on
 * that _id.
 * If we did not set it on mongos, mongod would generate an _id, but that generated _id might
 * actually mean the document belongs on a different shard. See SERVER-79914 for details.
 */
void addIdsForInserts(BulkWriteCommandRequest& origCmdRequest);

}  // namespace bulk_write_exec
}  // namespace mongo
