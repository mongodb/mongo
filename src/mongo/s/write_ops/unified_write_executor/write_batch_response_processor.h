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
#include "mongo/s/write_ops/unified_write_executor/unified_write_executor.h"
#include "mongo/s/write_ops/unified_write_executor/write_batch_executor.h"
#include "mongo/s/write_ops/wc_error.h"
#include "mongo/util/modules.h"

namespace mongo::unified_write_executor {

/**
 * A struct representing data to be returned to the higher level of control flow.
 */
struct ProcessorResult {
    using CollectionsToCreate =
        stdx::unordered_map<NamespaceString,
                            std::shared_ptr<const mongo::CannotImplicitlyCreateCollectionInfo>>;

    // Ops to be retried due to recoverable errors
    std::vector<WriteOp> opsToRetry;

    // Collections to be explicitly created
    CollectionsToCreate collsToCreate;

    // Shards where each op succeeded. This is used to track the shards that we don't need to retry
    // on because of retryable(staleness/collection doesn't exist errors) because the operation was
    // already executed.
    std::map<WriteOpId, std::set<ShardId>> successfulShardSet;
};

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
    struct BulkWriteOpResults {
        std::vector<BulkWriteReplyItem> items;
        bool hasSuccess = false;
        bool hasError = false;
    };

    /**
     * The 'Unexecuted' struct is used by 'ItemVariant' to represent an op that was scheduled
     * to execute on a shard but didn't execute.
     */
    struct Unexecuted {};

    /**
     * When the 'errorsOnly' parameter is true, the 'SucceededWithoutItem' struct is used by
     * 'ItemVariant' to represent an op that executed on a shard and completed succesfully
     * without producing an explicit "reply item".
     */
    struct SucceededWithoutItem {};

    using FindAndModifyReplyItem = StatusWith<write_ops::FindAndModifyCommandReply>;

    using OpResultVariant = std::variant<BulkWriteOpResults, FindAndModifyReplyItem>;

    using ItemVariant =
        std::variant<Unexecuted, SucceededWithoutItem, BulkWriteReplyItem, FindAndModifyReplyItem>;

    using CollectionsToCreate = ProcessorResult::CollectionsToCreate;

    using GroupItemsResult =
        std::pair<std::map<WriteOp, std::vector<std::pair<ShardId, ItemVariant>>>, bool>;

    struct ShardResult {
        boost::optional<BulkWriteCommandReply> bulkWriteReply;
        boost::optional<WriteConcernErrorDetail> wce;
        std::vector<std::pair<WriteOp, ItemVariant>> items;
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
    ProcessorResult onWriteBatchResponse(OperationContext* opCtx,
                                         RoutingContext& routingCtx,
                                         const WriteBatchResponse& response);

    /**
     * Turns gathered statistics into a command reply for the client. Consumes any pending reply
     * items.
     */
    WriteCommandResponse generateClientResponse(OperationContext* opCtx);

    BatchedCommandResponse generateClientResponseForBatchedCommand(OperationContext* opCtx);

    BulkWriteCommandReply generateClientResponseForBulkWriteCommand(OperationContext* opCtx);

    FindAndModifyCommandResponse generateClientResponseForFindAndModifyCommand();

    /**
     * This method is called by the scheduler to record target errors that occurred during batch
     * creation.
     */
    void recordTargetErrors(OperationContext* opCtx, const BatcherResult& batcherResult);

    /**
     * Records an error attributed to 'op', adding the error to _results and updating _nErrors.
     */
    void recordError(OperationContext* opCtx, const WriteOp& op, const Status& status);

    /**
     * Returns the number of errors recorded so far.
     */
    size_t getNumErrorsRecorded() const {
        return _nErrors;
    }

    /**
     * Returns the number of OK responses that have been processed so far.
     */
    size_t getNumOkItemsProcessed() const {
        return _numOkItemsProcessed;
    }

    /**
     * Returns true if we've exceeded the max reply size, false otherwise. If we have exceeded the
     * max size, we record this as an error for the next write op.
     */
    bool checkBulkWriteReplyMaxSize(OperationContext* opCtx);

private:
    ProcessorResult _onWriteBatchResponse(OperationContext* opCtx,
                                          RoutingContext& routingCtx,
                                          const EmptyBatchResponse& response);
    ProcessorResult _onWriteBatchResponse(OperationContext* opCtx,
                                          RoutingContext& routingCtx,
                                          const SimpleWriteBatchResponse& response);
    ProcessorResult _onWriteBatchResponse(OperationContext* opCtx,
                                          RoutingContext& routingCtx,
                                          const NoRetryWriteBatchResponse& response);

    /**
     * This method performs some initial processing on the specified ShardResponse object and
     * returns a ShardResult.
     *
     * If 'response' has a top-level error, this method will handle transient txn errors and
     * it will inform 'routingCtx' about stale errors, and it will generate item errors for
     * individual ops as appropriate. Then it will return a ShardResult containing the top-level
     * error and the generated item errors.
     *
     * If 'response' doesn't have a top-level erorr, this method will get all the reply items
     * (if they didn't all fit in the first batch), it will handle any stale errors in the reply
     * items, and it will determine which ops didn't execute (if any). If 'errorsOnly' is true,
     * this method will also determine which ops executed successfully that were not listed in the
     * reply items. This method will then return a ShardResult containing the parsed response, the
     * reply items, and info about which ops executed successfully and which ops didn't execute.
     */
    ShardResult onShardResponse(OperationContext* opCtx,
                                RoutingContext& routingCtx,
                                const ShardId& shardId,
                                const ShardResponse& response);

    /**
     * Process the counters and the list of retried stmtIds from the BulkWriteCommandReply. Note
     * that this method does not update _numOkResponses or _nErrors.
     */
    void processCountersAndRetriedStmtIds(const BulkWriteCommandReply& parsedReply);

    /**
     * Helper to keep _approximateSize up to date when appending to _replies.
     */
    void updateApproximateSize(const BulkWriteReplyItem& replyItem);

    /**
     * Iterates through all of the _results and combines all of the reply items for each op into
     * a single reply item. This is called when we are ready to generate the client response.
     */
    std::vector<std::pair<WriteOpId, boost::optional<BulkWriteReplyItem>>> finalizeRepliesForOps(
        OperationContext* opCtx);

    /**
     * Add a boost::optional<BulkWriteReplyItem> attributed to 'op' to _results, and also update
     * _numOkResponses and _nErrors appropriately.
     */
    void recordResult(OperationContext* opCtx,
                      const WriteOp& op,
                      boost::optional<BulkWriteReplyItem> item);

    /**
     * Add a FindAndModifyReplyItem attributed to 'op' to _results, and also update _numOkResponses
     * and _nErrors appropriately.
     */
    void recordResult(OperationContext* opCtx, const WriteOp& op, FindAndModifyReplyItem item);

    /**
     * Informs 'routingCtx' about the stale error given by 'status' if needed.
     */
    void noteRetryableError(OperationContext* opCtx,
                            RoutingContext& routingCtx,
                            boost::optional<WriteOp> op,
                            const Status& status);

    /**
     * Helper method that adds 'op' to 'toRetry' and also copies CannotImplicityCreateCollection
     * errors into 'collsToCreate'.
     */
    void queueOpForRetry(const WriteOp& op,
                         boost::optional<const Status&> status,
                         std::set<WriteOp>& toRetry,
                         CollectionsToCreate& collsToCreate) const;

    /**
     * Generate an ItemVariant for 'op' with an error given by 'status'.
     */
    ItemVariant makeErrorItem(const WriteOp& op, const Status& status) const;

    /**
     * This method scans the items from 'shardResults' and groups the items together by op. This
     * method also checks if 'shardResults' contains an unrecoverable error.
     */
    GroupItemsResult groupItemsByOp(
        OperationContext* opCtx, std::vector<std::pair<ShardId, ShardResult>>& shardResults) const;

    /**
     * Gets the BulkWriteReplyItems from 'result.bulkWriteReply' and puts them into 'result.items'.
     */
    void retrieveBulkWriteReplyItems(OperationContext* opCtx,
                                     RoutingContext& routingCtx,
                                     const ShardId& shardId,
                                     const std::vector<WriteOp>& ops,
                                     bool errorsOnly,
                                     ShardResult& result);

    /**
     * This method scans 'items' and verifies that the "idx" values are valid and that they appear
     * in increasing order. If 'errorsOnly' is false, this method also verifies that 'items' has
     * a reply item for each op that succeeded.
     */
    void validateBulkWriteReplyItems(OperationContext* opCtx,
                                     const std::vector<BulkWriteReplyItem>& items,
                                     size_t numOps,
                                     bool errorsOnly);

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
    size_t _numOkItemsProcessed{0};

    std::map<WriteOpId, OpResultVariant> _results;

    int32_t _approximateSize = 0;
    std::vector<ShardWCError> _wcErrors;
    stdx::unordered_set<StmtId> _retriedStmtIds;
};

}  // namespace mongo::unified_write_executor
