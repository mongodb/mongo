/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/cannot_implicitly_create_collection_info.h"
#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/s/write_ops/bulk_write_reply_info.h"
#include "mongo/s/write_ops/unified_write_executor/write_batch_executor.h"
#include "mongo/s/write_ops/wc_error.h"
#include "mongo/util/modules.h"

namespace mongo::unified_write_executor {
using WriteCommandResponse = std::variant<BatchedCommandResponse, BulkWriteCommandReply>;

/**
 * Handles responses from shards and interactions with the catalog necessary to retry certain
 * errors.
 * Intended workflow:
 *
 * WriteBatchResponseProcessor processor;
 * {
 *     RoutingContext rtx(...);
 *     WriteBatchResponse response = ...;
 *     auto [toRetry, collectionsToCreate] = processor.onWriteBatchResponse(response);
 *     // queue operations to be retried;
 * }
 * // It is important that the lifetime of RoutingContext has ended, since creating collections will
 * start a new routing operation
 * // Process more rounds of batches...
 * // Generate a response based on the original command we received.
 */
class WriteBatchResponseProcessor {
public:
    using CollectionsToCreate =
        stdx::unordered_map<NamespaceString,
                            std::shared_ptr<const mongo::CannotImplicitlyCreateCollectionInfo>>;

    /**
     * A struct representing data to be returned to the higher level of control flow.
     */
    struct Result {
        std::vector<WriteOp> opsToRetry;    // Ops to be retried due to recoverable errors
        CollectionsToCreate collsToCreate;  // Collections to be explicitly created
    };

    explicit WriteBatchResponseProcessor(WriteCommandRef cmdRef,
                                         Stats& stats,
                                         bool isNonVerbose = false,
                                         BSONObj originalCommand = BSONObj())
        : _cmdRef(cmdRef),
          _stats(stats),
          _isNonVerbose(isNonVerbose),
          _originalCommand(originalCommand) {}

    explicit WriteBatchResponseProcessor(const BatchedCommandRequest& request,
                                         Stats& stats,
                                         bool isNonVerbose = false,
                                         BSONObj originalCommand = BSONObj())
        : WriteBatchResponseProcessor(
              WriteCommandRef{request}, stats, isNonVerbose, originalCommand) {}

    explicit WriteBatchResponseProcessor(const BulkWriteCommandRequest& request,
                                         Stats& stats,
                                         bool isNonVerbose = false,
                                         BSONObj originalCommand = BSONObj())
        : WriteBatchResponseProcessor(
              WriteCommandRef{request}, stats, isNonVerbose, originalCommand) {}

    /**
     * Process a response from each shard, handle errors, and collect statistics. Returns an
     * array containing ops that did not complete successfully that need to be resent.
     *
     * If 'response' is a type that can stale errors (such as SimpleWriteBatchResponse), then
     * 'routingCtx' must be an initialized RoutingContext.
     */
    Result onWriteBatchResponse(OperationContext* opCtx,
                                RoutingContext& routingCtx,
                                const WriteBatchResponse& response);

    /**
     * Turns gathered statistics into a command reply for the client. Consumes any pending reply
     * items.
     */
    WriteCommandResponse generateClientResponse(OperationContext* opCtx);

    BatchedCommandResponse generateClientResponseForBatchedCommand();

    BulkWriteCommandReply generateClientResponseForBulkWriteCommand(OperationContext* opCtx);

    /**
     * This method is called by the scheduler to record a target error that occurred during batch
     * creation.
     */
    void recordTargetError(OperationContext* opCtx, const WriteOp& op, const Status& status);

    /**
     * This method is called by the scheduler to record errors for the remaining ops that don't have
     * results yet. If the write command is ordered or running in a transaction, this method will
     * only record one error (for the remaining op with the lowest ID). Otherwise, this method will
     * record errors for all remaining ops.
     */
    void recordErrorForRemainingOps(OperationContext* opCtx, const Status& status);

    /**
     * Returns the number of errors recorded so far.
     */
    size_t getNumErrorsRecorded() const {
        return _nErrors;
    }

    /**
     * Returns the number of OK responses that have been processed so far.
     */
    size_t getNumOkResponsesProcessed() const {
        return _numOkResponses;
    }

private:
    Result _onWriteBatchResponse(OperationContext* opCtx,
                                 RoutingContext& routingCtx,
                                 const EmptyBatchResponse& response);
    Result _onWriteBatchResponse(OperationContext* opCtx,
                                 RoutingContext& routingCtx,
                                 const SimpleWriteBatchResponse& response);
    Result _onWriteBatchResponse(OperationContext* opCtx,
                                 RoutingContext& routingCtx,
                                 const NoRetryWriteBatchResponse& response);

    /**
     * Process a response from a shard, handle errors, and collect statistics. Returns an array
     * containing ops that did not complete successfully that need to be resent.
     */
    Result onShardResponse(OperationContext* opCtx,
                           RoutingContext& routingCtx,
                           const ShardId& shardId,
                           const ShardResponse& response);

    /**
     * Process ReplyItems and pick out any ops that need to be retried.
     */
    Result processOpsInReplyItems(OperationContext* opCtx,
                                  RoutingContext& routingCtx,
                                  const std::vector<WriteOp>& ops,
                                  const std::vector<BulkWriteReplyItem>&);

    /**
     * If an op was not in the ReplyItems, this function processes it and decides if a retry is
     * needed.
     */
    std::vector<WriteOp> processOpsNotInReplyItems(const std::vector<WriteOp>& requestedOps,
                                                   const std::vector<BulkWriteReplyItem>&,
                                                   std::vector<WriteOp>&& toRetry);

    /**
     * Process a single ReplyItem attributed to 'op', adding it to _results and updating
     * _numOkResponses and _nErrors.
     */
    void processReplyItem(const WriteOp& op, BulkWriteReplyItem item);

    /**
     * Process an error attributed to 'op', adding it to _results and updating _nErrors.
     */
    void processError(const WriteOp& op, const Status& status);

    /**
     * Process a local or top-level error attributed to an entire batch of WriteOps ('ops'),
     * updating _results and _nErrors appropriately.
     */
    void processErrorForBatch(OperationContext* opCtx,
                              const std::vector<WriteOp>& ops,
                              const Status& status);

    /**
     * If the write command is running in a transaction and 'status' is a transient transaction
     * error, then this method returns 'status'. Otherwise this method returns boost::none.
     */
    boost::optional<Status> getTransientTxnError(OperationContext* opCtx, const Status& status);

    /**
     * If the write command is running in a transaction and 'status' is a transient transaction
     * error, then this method returns 'status' with some additional context added. Otherwise this
     * method returns boost::none.
     */
    boost::optional<Status> getTransientTxnError(
        OperationContext* opCtx,
        const executor::RemoteCommandResponse& shardResponse,
        const Status& status);

    /**
     * Returns the retriedStmtIds to set them in the client response.
     */
    std::vector<StmtId> getRetriedStmtIds() const {
        std::vector<StmtId> retriedStmtIds{_retriedStmtIds.begin(), _retriedStmtIds.end()};
        return retriedStmtIds;
    };

    WriteCommandRef _cmdRef;
    Stats& _stats;
    const bool _isNonVerbose;
    BSONObj _originalCommand;
    size_t _nErrors{0};
    size_t _nInserted{0};
    size_t _nMatched{0};
    size_t _nModified{0};
    size_t _nUpserted{0};
    size_t _nDeleted{0};
    size_t _numOkResponses{0};
    std::map<WriteOpId, BulkWriteReplyItem> _results;
    std::vector<ShardWCError> _wcErrors;
    stdx::unordered_set<StmtId> _retriedStmtIds;
};

}  // namespace mongo::unified_write_executor
