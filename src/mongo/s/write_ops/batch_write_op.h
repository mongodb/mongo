/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <map>
#include <set>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/ns_targeter.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/write_op.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {

class OperationContext;
class TrackedErrors;

// Conservative overhead per element contained in the write batch. This value was calculated as 1
// byte (element type) + 5 bytes (max string encoding of the array index encoded as string and the
// maximum key is 99999) + 1 byte (zero terminator) = 7 bytes
const int kWriteCommandBSONArrayPerElementOverheadBytes = 7;

/**
 * Simple struct for storing an error with an endpoint.
 *
 * Certain types of errors are not stored in WriteOps or must be returned to a caller.
 */
struct ShardError {
    ShardError(const ShardEndpoint& endpoint, const write_ops::WriteError& error)
        : endpoint(endpoint), error(error) {}

    ShardEndpoint endpoint;
    write_ops::WriteError error;
};

/**
 * Simple struct for storing a write concern error with an endpoint.
 *
 * Certain types of errors are not stored in WriteOps or must be returned to a caller.
 */
struct ShardWCError {
    ShardWCError(const ShardEndpoint& endpoint, const WriteConcernErrorDetail& error)
        : endpoint(endpoint) {
        error.cloneTo(&this->error);
    }

    ShardEndpoint endpoint;
    WriteConcernErrorDetail error;
};

using TargetedBatchMap = std::map<ShardId, std::unique_ptr<TargetedWriteBatch>>;

/**
 * The BatchWriteOp class manages the lifecycle of a batched write received by mongos.  Each
 * item in a batch is tracked via a WriteOp, and the function of the BatchWriteOp is to
 * aggregate the dispatched requests and responses for the underlying WriteOps.
 *
 * Overall, the BatchWriteOp lifecycle is similar to the WriteOp lifecycle, with the following
 * stages:
 *
 * 0) Client request comes in, batch write op is initialized
 *
 * 1a) One or more ops in the batch are targeted using targetBatch, resulting in
 *     TargetedWriteBatches for these ops.
 * 1b) There are targeting errors, and the batch must be retargeted after refreshing the
 *     NSTargeter.
 *
 * 2) (Child BatchCommandRequests are be built for each TargetedWriteBatch before sending)
 *
 * 3) Responses for sent TargetedWriteBatches are noted, errors are stored and aggregated per-
 *    write-op.  Errors the caller is interested in are returned.
 *
 * 4) If the batch write is not finished, goto 0
 *
 * 5) When all responses come back for all write ops, errors are aggregated and returned in
 *    a client response
 *
 */
class BatchWriteOp {
    BatchWriteOp(const BatchWriteOp&) = delete;
    BatchWriteOp& operator=(const BatchWriteOp&) = delete;

public:
    BatchWriteOp(OperationContext* opCtx, const BatchedCommandRequest& clientRequest);
    ~BatchWriteOp() = default;

    /**
     * Targets one or more of the next write ops in this batch op using a NSTargeter.  The
     * resulting TargetedWrites are aggregated together in the returned TargetedWriteBatches.
     *
     * If 'recordTargetErrors' is false, any targeting error will abort all current batches and
     * the method will return the targeting error.  No targetedBatches will be returned on
     * error.
     *
     * Otherwise, if 'recordTargetErrors' is true, targeting errors will be recorded for each
     * write op that fails to target, and the method will return OK.
     *
     * (The idea here is that if we are sure our NSTargeter is up-to-date we should record
     * targeting errors, but if not we should refresh once first.)
     *
     * Returned TargetedWriteBatches are owned by the caller.
     * If a write without a shard key is detected, return an OK StatusWith that has 'true' as the
     * value.
     */
    StatusWith<bool> targetBatch(const NSTargeter& targeter,
                                 bool recordTargetErrors,
                                 TargetedBatchMap* targetedBatches);

    /**
     * Fills a BatchCommandRequest from a TargetedWriteBatch for this BatchWriteOp.
     */
    BatchedCommandRequest buildBatchRequest(
        const TargetedWriteBatch& targetedBatch,
        const NSTargeter& targeter,
        boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery) const;

    /**
     * Stores a response from one of the outstanding TargetedWriteBatches for this BatchWriteOp.
     * The response may be in any form, error or not.
     *
     * There is an additional optional 'trackedErrors' parameter, which can be used to return
     * copies of any write errors in the response that the caller is interested in (specified by
     * errCode).  (This avoids external callers needing to know much about the response format.)
     */
    void noteBatchResponse(const TargetedWriteBatch& targetedBatch,
                           const BatchedCommandResponse& response,
                           TrackedErrors* trackedErrors);

    /**
     * Stores an error that occurred trying to send/recv a TargetedWriteBatch for this
     * BatchWriteOp.
     */
    void noteBatchError(const TargetedWriteBatch& targetedBatch,
                        const write_ops::WriteError& error);

    /**
     * Aborts any further writes in the batch with the provided error.  There must be no pending
     * ops awaiting results when a batch is aborted.
     *
     * Batch is finished immediately after aborting.
     */
    void abortBatch(const write_ops::WriteError& error);

    /**
     * Disposes of all tracked targeted batches when an error is encountered during a transaction.
     * This is safe because any partially written data on shards will be rolled back if mongos
     * decides to abort.
     */
    void forgetTargetedBatchesOnTransactionAbortingError();

    /**
     * Returns false if the batch write op needs more processing.
     */
    bool isFinished();

    /**
     * Fills a batch response to send back to the client.
     */
    void buildClientResponse(BatchedCommandResponse* batchResp);

    /**
     * Returns the number of write operations which are in the specified state. Runs in O(number of
     * write operations).
     */
    int numWriteOpsIn(WriteOpState state) const;

    boost::optional<int> getNShardsOwningChunks();

private:
    /**
     * Maintains the batch execution statistics when a response is received.
     */
    void _incBatchStats(const BatchedCommandResponse& response);

    /**
     * Helper function to cancel all the write ops of targeted batches in a map.
     */
    void _cancelBatches(const write_ops::WriteError& why, TargetedBatchMap&& batchMapToCancel);

    OperationContext* const _opCtx;

    // The incoming client request
    const BatchedCommandRequest& _clientRequest;

    // Cached transaction number (if one is present on the operation contex)
    boost::optional<TxnNumber> _batchTxnNum;

    // Array of ops being processed from the client request
    std::vector<WriteOp> _writeOps;

    // Current outstanding batch op write requests
    // Not owned here but tracked for reporting
    std::set<const TargetedWriteBatch*> _targeted;

    // Write concern responses from all write batches so far
    std::vector<ShardWCError> _wcErrors;

    // Upserted ids for the whole write batch
    std::vector<std::unique_ptr<BatchedUpsertDetail>> _upsertedIds;

    // Statement ids for the ops that had already been executed, thus were not executed in this
    // batch write.
    std::vector<StmtId> _retriedStmtIds;

    // Stats for the entire batch op
    int _numInserted{0};
    int _numUpserted{0};
    int _numMatched{0};
    int _numModified{0};
    int _numDeleted{0};

    // Set to true if this write is part of a transaction.
    const bool _inTransaction{false};
    const bool _isRetryableWrite{false};

    boost::optional<int> _nShardsOwningChunks;
};

/**
 * Helper class for tracking certain errors from batch operations
 */
class TrackedErrors {
public:
    TrackedErrors() = default;

    void startTracking(int errCode);

    bool isTracking(int errCode) const;

    void addError(ShardError error);

    const std::vector<ShardError>& getErrors(int errCode) const;

private:
    using TrackedErrorMap = stdx::unordered_map<int, std::vector<ShardError>>;
    TrackedErrorMap _errorMap;
};

typedef std::function<const NSTargeter&(const WriteOp& writeOp)> GetTargeterFn;
typedef std::function<int(const WriteOp& writeOp)> GetWriteSizeFn;

// Helper function to target ready writeOps. See BatchWriteOp::targetBatch for details.
StatusWith<bool> targetWriteOps(OperationContext* opCtx,
                                std::vector<WriteOp>& writeOps,
                                bool ordered,
                                bool recordTargetErrors,
                                GetTargeterFn getTargeterFn,
                                GetWriteSizeFn getWriteSizeFn,
                                TargetedBatchMap& batchMap);

}  // namespace mongo
