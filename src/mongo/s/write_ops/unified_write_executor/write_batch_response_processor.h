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

#include "mongo/s/cannot_implicitly_create_collection_info.h"
#include "mongo/s/cluster_ddl.h"
#include "mongo/s/write_ops/unified_write_executor/write_batch_executor.h"
#include "mongo/s/write_ops/wc_error.h"

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
     * An enum that represents the type of error encountered.
     */
    enum class ErrorType {
        // No error occurred, continue with next batch.
        kNone,
        // Finish processing the current responses but ignore subsequent batches.
        kUnrecoverable,
        // Halt further processing and return immediately.
        kStopProcessing,
    };

    /**
     * A struct representing data to be returned to the higher level of control flow.
     */
    struct Result {
        ErrorType errorType{ErrorType::kNone};  // Type of error encountered if any.
        std::vector<WriteOp> opsToRetry{};      // Ops to be retried due to recoverable errors
        CollectionsToCreate collsToCreate{};    // Collections to be explicitly created
    };

    explicit WriteBatchResponseProcessor(WriteCommandRef cmdRef,
                                         Stats& stats,
                                         bool isNonVerbose = false)
        : _cmdRef(cmdRef), _stats(stats), _isNonVerbose(isNonVerbose) {}

    explicit WriteBatchResponseProcessor(const BatchedCommandRequest& request,
                                         Stats& stats,
                                         bool isNonVerbose = false)
        : WriteBatchResponseProcessor(WriteCommandRef{request}, stats, isNonVerbose) {}

    explicit WriteBatchResponseProcessor(const BulkWriteCommandRequest& request,
                                         Stats& stats,
                                         bool isNonVerbose = false)
        : WriteBatchResponseProcessor(WriteCommandRef{request}, stats, isNonVerbose) {}

    /**
     * Process a response from each shard, handle errors, and collect statistics. Returns an
     * array containing ops that did not complete successfully that need to be resent.
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

private:
    Result _onWriteBatchResponse(OperationContext* opCtx,
                                 RoutingContext& routingCtx,
                                 const SimpleWriteBatchResponse& response);
    Result _onWriteBatchResponse(OperationContext* opCtx,
                                 RoutingContext& routingCtx,
                                 const NonTargetedWriteBatchResponse& response);

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
     * Handle errors when we do not have responses from the remote shards.
     */
    Result handleLocalError(OperationContext* opCtx,
                            Status status,
                            WriteOp op,
                            boost::optional<const ShardId&> shardId);

    /**
     * Adds a BulkReplyItem with the opId and status to '_results' and increments '_nErrors'. Used
     * when we need to abort the whole batch due to an error when we're in a transaction.
     */
    void noteErrorResponseOnAbort(int opId, const Status& status);

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
    size_t _nErrors{0};
    size_t _nInserted{0};
    size_t _nMatched{0};
    size_t _nModified{0};
    size_t _nUpserted{0};
    size_t _nDeleted{0};
    std::map<WriteOpId, BulkWriteReplyItem> _results;
    std::vector<ShardWCError> _wcErrors;
    stdx::unordered_set<StmtId> _retriedStmtIds;
};

}  // namespace mongo::unified_write_executor
