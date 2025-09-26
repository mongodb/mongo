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

#include "mongo/s/write_ops/unified_write_executor/write_batch_response_processor.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/global_catalog/router_role_api/collection_uuid_mismatch.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/commands/query_cmd/populate_cursor.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/unified_write_executor/write_batch_executor.h"
#include "mongo/s/write_ops/write_op_helper.h"
#include "mongo/util/assert_util.h"

#include <variant>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::unified_write_executor {
using Result = WriteBatchResponseProcessor::Result;

static ErrorCodes::Error getStatusCode(const BulkWriteReplyItem& item) {
    return item.getStatus().code();
}

namespace {
bool isTransientTxnError(const Status& status, bool inTransaction) {
    return (!status.isOK() && inTransaction &&
            isTransientTransactionError(
                status.code(), false /*hasWriteConcernError*/, false /*isCommitOrAbort*/));
}

bool isTransientTxnError(const Status& status,
                         bool inTransaction,
                         const executor::RemoteCommandResponse& response) {
    return (!status.isOK() && inTransaction &&
            hasTransientTransactionErrorLabel(
                ErrorReply::parse(response.data, IDLParserContext("ErrorReply"))));
}
}  // namespace

Result WriteBatchResponseProcessor::onWriteBatchResponse(OperationContext* opCtx,
                                                         RoutingContext& routingCtx,
                                                         const WriteBatchResponse& response) {
    return std::visit(
        [&](const auto& responseData) -> Result {
            return _onWriteBatchResponse(opCtx, routingCtx, responseData);
        },
        response);
}

Result WriteBatchResponseProcessor::_onWriteBatchResponse(OperationContext* opCtx,
                                                          RoutingContext& routingCtx,
                                                          const EmptyBatchResponse& response) {
    return Result{};
}

Result WriteBatchResponseProcessor::_onWriteBatchResponse(
    OperationContext* opCtx, RoutingContext& routingCtx, const SimpleWriteBatchResponse& response) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));

    Result result;

    for (const auto& [shardId, shardResponse] : response) {
        auto shardResult = onShardResponse(opCtx, routingCtx, shardId, shardResponse);
        result.combine(std::move(shardResult));

        // If this command is running in a transaction and an error occurs, stop processing
        // immediately and return a Result for what we've processed so far.
        if (_nErrors > 0 && inTransaction) {
            break;
        }
    }

    removeFailedOpsFromOpsToRetry(result);
    return result;
}

Result WriteBatchResponseProcessor::_onWriteBatchResponse(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    const NoRetryWriteBatchResponse& response) {
    // TODO SERVER-104122 Support for 'WouldChangeOwningShard' writes.
    // TODO SERVER-105762 Add support for errorsOnly: true.
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    const auto& swRes = response.swResponse;
    const auto& op = response.op;

    if (!swRes.isOK()) {
        // TODO SERVER-105303 Handle interruption/shutdown.
        // TODO SERVER-104122 Support for 'WouldChangeOwningShard' writes.
        LOGV2_DEBUG(10896500,
                    4,
                    "Cluster write op executing in internal transaction failed with error",
                    "error"_attr = redact(swRes.getStatus()));

        // Process the local or top-level error for the batch.
        processErrorForBatch(opCtx, std::vector{op}, swRes.getStatus(), boost::none);

        // If the write command is running in a transaction and there was a transient transaction
        // error, uassert so that the error is returned directly as a top level error, to allow the
        // client to retry.
        if (isTransientTxnError(swRes.getStatus(), inTransaction)) {
            uassertStatusOK(swRes.getStatus());
        }

        if (inTransaction) {
            LOGV2_DEBUG(10413100,
                        4,
                        "Aborting write command due to error in a transaction",
                        "error"_attr = redact(swRes.getStatus()));
        }

        return Result{};
    }

    const BulkWriteCommandReply& parsedReply = swRes.getValue();

    // Process write concern error (if any).
    if (response.wce) {
        _wcErrors.push_back(ShardWCError{std::move(*response.wce)});
    }

    // Update the counters.
    _nInserted += parsedReply.getNInserted();
    _nDeleted += parsedReply.getNDeleted();
    _nMatched += parsedReply.getNMatched();
    _nUpserted += parsedReply.getNUpserted();
    _nModified += parsedReply.getNModified();

    // Update the list of retried stmtIds.
    if (auto retriedStmtIds = parsedReply.getRetriedStmtIds();
        retriedStmtIds && !retriedStmtIds->empty()) {
        for (auto retriedStmtId : *retriedStmtIds) {
            _retriedStmtIds.insert(retriedStmtId);
        }
    }

    const auto& replyItems = parsedReply.getCursor().getFirstBatch();
    tassert(10378000, "Unexpected reply for NoRetryWriteBatchResponse", replyItems.size() == 1);

    const auto& replyItem = replyItems.front();
    tassert(
        10378001,
        fmt::format("reply with invalid opId {} when command only had 1 op", replyItem.getIdx()),
        static_cast<WriteOpId>(replyItem.getIdx()) == 0);

    tassert(11151300,
            "Unexpected reply item error for NoRetryWriteBatchResponse",
            replyItem.getStatus().isOK());

    // Process the reply item.
    processReplyItem(op, std::move(replyItem), boost::none);

    // Batch types that produce NoRetryWriteBatchResponse are executed using a mechanism that deals
    // with stale errors and retrying internally, so 'opsToRetry' will always be empty.
    //
    // Likewise, batch types that produce NoRetryWriteBatchResponse cannot perform inserts, so
    // 'collsToCreate' will always be empty as well.
    return Result{};
}

Result WriteBatchResponseProcessor::handleRetryableError(OperationContext* opCtx,
                                                         RoutingContext& routingCtx,
                                                         WriteOp op,
                                                         const Status& status) {

    WriteBatchResponseProcessor::CollectionsToCreate collectionsToCreate;
    const auto itemCode = status.code();
    if (itemCode == ErrorCodes::CannotImplicitlyCreateCollection) {
        // Stage the collection to be created if it was found to not exist.
        auto info = status.extraInfo<CannotImplicitlyCreateCollectionInfo>();
        if (auto it = collectionsToCreate.find(info->getNss()); it == collectionsToCreate.cend()) {
            collectionsToCreate.emplace(info->getNss(), std::move(info));
        }
    } else if (itemCode == ErrorCodes::StaleDbVersion ||
               ErrorCodes::isStaleShardVersionError(itemCode)) {
        if (itemCode == ErrorCodes::StaleDbVersion) {
            LOGV2_DEBUG(10411403, 4, "Noting stale database response", "status"_attr = status);
        } else {
            LOGV2_DEBUG(10346900, 4, "Noting stale config response", "status"_attr = status);
        }
        routingCtx.onStaleError(status, op.getNss());
    } else if (itemCode == ErrorCodes::ShardCannotRefreshDueToLocksHeld) {
        LOGV2_DEBUG(10413104,
                    4,
                    "Noting shard cannot refresh due to locks held response",
                    "status"_attr = status);
    }

    return {{op}, std::move(collectionsToCreate), {}};
}

void WriteBatchResponseProcessor::removeFailedOpsFromOpsToRetry(Result& result) {

    // Remove ops with non-retryable errors from the retry list. Ops with non-retryable errors are
    // considered complete.
    for (auto& [opId, opResult] : _results) {
        if (opResult.hasNonRetryableError) {
            result.opsToRetry.erase(std::remove(result.opsToRetry.begin(),
                                                result.opsToRetry.end(),
                                                _cmdRef.getOp(opId)),
                                    result.opsToRetry.end());
        }
    }
}

Result WriteBatchResponseProcessor::onShardResponse(OperationContext* opCtx,
                                                    RoutingContext& routingCtx,
                                                    const ShardId& shardId,
                                                    const ShardResponse& response) {
    const Status& status = response.swResponse.getStatus();
    const auto& ops = response.ops;
    const boost::optional<HostAndPort>& hostAndPort = response.shardHostAndPort;
    const bool ordered = _cmdRef.getOrdered();
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));

    if (!status.isOK()) {
        // TODO SERVER-105303 Handle interruption/shutdown.
        // TODO SERVER-104122 Support for 'WouldChangeOwningShard' writes.
        LOGV2_DEBUG(10896501,
                    4,
                    "Unable to receive cluster write results from shard",
                    "error"_attr = redact(status),
                    "shardId"_attr = shardId.toString(),
                    "host"_attr = (hostAndPort ? hostAndPort->toString() : std::string("(none)")));

        // Process the local error for the batch.
        processErrorForBatch(opCtx, ops, status, shardId);

        // If the write command is running in a transaction and there was a transient transaction
        // error, uassert so that the error is returned directly as a top level error, to allow the
        // client to retry.
        if (isTransientTxnError(status, inTransaction)) {
            uassertStatusOK(status);
        }

        if (inTransaction) {
            LOGV2_DEBUG(10896502,
                        4,
                        "Aborting write command due to error in a transaction",
                        "error"_attr = redact(status),
                        "host"_attr =
                            (hostAndPort ? hostAndPort->toString() : std::string("(none)")));
        }

        return Result{};
    }

    auto shardResponse = response.swResponse.getValue();
    LOGV2_DEBUG(10347003,
                4,
                "Processing cluster write shard response",
                "response"_attr = shardResponse.data,
                "host"_attr = shardResponse.target);

    // Handle any top level errors.
    auto shardResponseStatus = getStatusFromCommandResult(shardResponse.data);
    if (!shardResponseStatus.isOK()) {
        auto status = shardResponseStatus.withContext(
            str::stream() << "cluster write results unavailable from " << shardResponse.target);

        if (status == ErrorCodes::StaleDbVersion || ErrorCodes::isStaleShardVersionError(status)) {
            // Inform the RoutingContext that a stale error occurred.
            routingCtx.onStaleError(status);
        }

        LOGV2_DEBUG(10347001,
                    4,
                    "Remote error reported from shard when receiving cluster write results",
                    "error"_attr = redact(status),
                    "host"_attr = shardResponse.target);

        // Process the top-level error for the batch.
        processErrorForBatch(opCtx, ops, status, shardId);

        // If the write command is running in a transaction and there was a transient transaction
        // error, uassert so that the error is returned directly as a top level error, to allow the
        // client to retry.
        if (isTransientTxnError(status, inTransaction, shardResponse)) {
            uassertStatusOK(status.withContext(str::stream()
                                               << "Encountered error from " << shardResponse.target
                                               << " during a transaction"));
        }

        if (inTransaction) {
            LOGV2_DEBUG(10413101,
                        4,
                        "Aborting write command due to error in a transaction",
                        "error"_attr = redact(status),
                        "host"_attr = shardResponse.target);
        }

        return Result{};
    }

    // Parse and handle inner ok and error responses.
    auto parsedReply = BulkWriteCommandReply::parse(
        shardResponse.data, IDLParserContext("BulkWriteCommandReply_UnifiedWriteExec"));

    // Process write concern error (if any).
    auto wcError = parsedReply.getWriteConcernError();
    if (wcError) {
        _wcErrors.push_back(ShardWCError(
            shardId, {Status(ErrorCodes::Error(wcError->getCode()), wcError->getErrmsg())}));
    }

    // Update the counters.
    _nInserted += parsedReply.getNInserted();
    _nDeleted += parsedReply.getNDeleted();
    _nMatched += parsedReply.getNMatched();
    _nUpserted += parsedReply.getNUpserted();
    _nModified += parsedReply.getNModified();

    // Update the list of retried stmtIds.
    if (auto retriedStmtIds = parsedReply.getRetriedStmtIds();
        retriedStmtIds && !retriedStmtIds->empty()) {
        for (auto retriedStmtId : *retriedStmtIds) {
            _retriedStmtIds.insert(retriedStmtId);
        }
    }

    const auto replyItems = exhaustCursorForReplyItems(opCtx, shardId, parsedReply);

    auto result = processOpsInReplyItems(opCtx, routingCtx, shardId, ops, replyItems);

    if (_nErrors == 0 || (!ordered && !inTransaction)) {
        result.opsToRetry =
            processOpsNotInReplyItems(ops, replyItems, std::move(result.opsToRetry));
    }

    return result;
}


void WriteBatchResponseProcessor::addReplyToResults(WriteOpId opId,
                                                    BulkWriteReplyItem reply,
                                                    boost::optional<ShardId> shardId) {
    bool hasNonRetryableError =
        !reply.getStatus().isOK() && !write_op_helpers::isRetryErrCode(reply.getStatus().code());

    if (!shardId) {
        _results[opId].hasNonRetryableError |= hasNonRetryableError;
        _results[opId].replies = reply;
        return;
    }

    ReplyItemsByShard replyItemShardMap{{*shardId, reply}};
    auto [it, inserted] =
        _results.emplace(opId, WriteOpResults{std::move(replyItemShardMap), hasNonRetryableError});

    tassert(10412305,
            "Expected replies to be of type shardId to map",
            (std::holds_alternative<ReplyItemsByShard>(it->second.replies)));

    auto& repliesMap = std::get<ReplyItemsByShard>(it->second.replies);

    if (inserted) {
        _results[opId].hasNonRetryableError |= hasNonRetryableError;
        repliesMap[*shardId].setIdx(opId);
    } else {
        // Add the reply item to the map, overwriting the previous reply for the same shard if there
        // already is one. If there is a previous reply for the same shard, we expect it to be a
        // retryable error.
        auto it = repliesMap.find(*shardId);
        if (it != repliesMap.end()) {
            tassert(10412308,
                    "Expected previous reply for the same shard to be a retryable error",
                    write_op_helpers::isRetryErrCode(it->second.getStatus().code()));
        }

        _results[opId].hasNonRetryableError |= hasNonRetryableError;
        repliesMap[*shardId] = reply;
        repliesMap[*shardId].setIdx(opId);
    }
    return;
}

void WriteBatchResponseProcessor::processReplyItem(const WriteOp& op,
                                                   BulkWriteReplyItem item,
                                                   boost::optional<ShardId> shardId) {
    const bool isOK = item.getStatus().isOK();

    // Set the "idx" field to the ID of 'op'.
    item.setIdx(op.getId());

    // If this is an update op, ensure the "nModified" field is set.
    if (op.getType() == WriteType::kUpdate && !item.getNModified()) {
        item.setNModified(0);
    }

    addReplyToResults(op.getId(), item, shardId);

    if (isOK) {
        _numOkResponses++;
    } else if (!write_op_helpers::isRetryErrCode(item.getStatus().code())) {
        _nErrors++;
    }
}

void WriteBatchResponseProcessor::processError(const WriteOp& op,
                                               const Status& status,
                                               boost::optional<ShardId> shardId) {
    tassert(10896503, "Unexpectedly got an OK status", !status.isOK());
    processReplyItem(op, BulkWriteReplyItem(0, status), shardId);
}

void WriteBatchResponseProcessor::processErrorForBatch(OperationContext* opCtx,
                                                       const std::vector<WriteOp>& ops,
                                                       const Status& status,
                                                       boost::optional<ShardId> shardId) {
    if (_cmdRef.getOrdered() || TransactionRouter::get(opCtx)) {
        // If the write command is ordered or running in a transaction -AND- if no errors have been
        // recorded yet, then record an error for the op with the lowest ID only.
        if (_nErrors == 0 && !ops.empty()) {
            const auto& firstOp = *std::min_element(ops.begin(), ops.end());
            processError(firstOp, status, shardId);
        }
    } else {
        // If the write command is unordered and not in a transaciton, record an error for each
        // op in 'ops'.
        for (const auto& op : ops) {
            processError(op, status, shardId);
        }
    }
}

BulkWriteReplyItem combineSuccessfulReplies(WriteOpId opId,
                                            std::vector<BulkWriteReplyItem> successfulReplies) {

    if (successfulReplies.size() == 1) {
        return successfulReplies.front();
    }

    BulkWriteReplyItem combinedReply;
    combinedReply.setOk(1);
    combinedReply.setIdx(opId);

    for (const auto& reply : successfulReplies) {
        if (auto n = reply.getN(); n.has_value()) {
            combinedReply.setN(combinedReply.getN().get_value_or(0) + n.value());
        }
        if (auto nModified = reply.getNModified(); nModified.has_value()) {
            combinedReply.setNModified(combinedReply.getNModified().get_value_or(0) +
                                       nModified.value());
        }
        if (auto upserted = reply.getUpserted(); upserted.has_value()) {
            tassert(10412300,
                    "Unexpectedly got bulkWrite upserted replies from multiple shards for a "
                    "single update operation",
                    !combinedReply.getUpserted().has_value());
            combinedReply.setUpserted(reply.getUpserted());
        }
    }

    return combinedReply;
}

BulkWriteReplyItem combineErrorReplies(WriteOpId opId,
                                       std::vector<BulkWriteReplyItem> errorReplies) {

    // Special case if there is only one error reply, they are all same, or we only have one
    // non-retryable error.
    if (errorReplies.size() == 1 || write_op_helpers::errorsAllSame(errorReplies, getStatusCode)) {
        return errorReplies.front();
    } else if (write_op_helpers::hasOnlyOneNonRetryableError(errorReplies, getStatusCode)) {
        return write_op_helpers::getFirstNonRetryableError(errorReplies, getStatusCode);
    }

    bool skipRetryableErrors =
        !write_op_helpers::hasAnyNonRetryableError(errorReplies, getStatusCode);

    // Generate the multi-error message below.
    std::stringstream msg("multiple errors for op : ");
    bool firstError = true;
    BSONArrayBuilder errB;
    for (std::vector<BulkWriteReplyItem>::const_iterator it = errorReplies.begin();
         it != errorReplies.end();
         ++it) {
        const BulkWriteReplyItem& errReply = *it;
        auto writeError = write_ops::WriteError(errReply.getIdx(), errReply.getStatus());
        if (!skipRetryableErrors ||
            !write_op_helpers::isRetryErrCode(errReply.getStatus().code())) {
            if (!firstError) {
                msg << " :: and :: ";
            }
            msg << errReply.getStatus().reason();
            errB.append(writeError.serialize());
            firstError = false;
        }
    }

    return BulkWriteReplyItem(opId, Status(MultipleErrorsOccurredInfo(errB.arr()), msg.str()));
}

Result WriteBatchResponseProcessor::processOpsInReplyItems(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    const ShardId shardId,
    const std::vector<WriteOp>& ops,
    const std::vector<BulkWriteReplyItem>& replyItems) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    Result result;

    for (const auto& item : replyItems) {
        // TODO SERVER-104122 Support for 'WouldChangeOwningShard' writes. if in transaction, return
        // an insert+delete to retry. if not in a transaction, return update to retry in
        // transaction.
        tassert(10347004,
                fmt::format("shard replied with invalid opId {} when it was only sent {} ops",
                            item.getIdx(),
                            ops.size()),
                static_cast<WriteOpId>(item.getIdx()) < ops.size());
        const auto& op = ops[item.getIdx()];

        auto status = item.getStatus();

        if (status.isOK()) {
            result.successfulShardSet[op.getId()].insert(shardId);
        } else if (write_op_helpers::isRetryErrCode(status.code())) {
            // If we got a retryable error, we process it accordingly. We don't need to record the
            // result from this shard as it'll be retried anyway.
            auto retryResult = handleRetryableError(opCtx, routingCtx, op, item.getStatus());
            result.combine(std::move(retryResult));
            continue;
        }

        processReplyItem(op, item, shardId);

        // Attempts to populate the actualCollection field of a CollectionUUIDMismatch>
        // if not already present.
        if (status.code() == ErrorCodes::CollectionUUIDMismatch) {
            status = populateCollectionUUIDMismatch(opCtx, status);
        }

        // If an error occurred and we are in a transaction, we stop processing and return the
        // first error.
        if (_nErrors > 0 && inTransaction) {
            LOGV2_DEBUG(10413103,
                        4,
                        "Aborting write command due to error in a transaction",
                        "error"_attr = redact(status));
            break;
        }
    }

    return result;
}

std::vector<WriteOp> WriteBatchResponseProcessor::processOpsNotInReplyItems(
    const std::vector<WriteOp>& requestedOps,
    const std::vector<BulkWriteReplyItem>& replyItems,
    std::vector<WriteOp>&& toRetry) {
    if (requestedOps.size() > replyItems.size()) {
        // TODO SERVER-105762 Add support for errorsOnly: true.
        // If we are here it means we got a response from an ordered: true command and it stopped on
        // the first error.
        const bool logOps = shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, logv2::LogSeverity::Debug(4));
        std::stringstream opsStream;
        size_t numOpsInStream = 0;

        for (size_t i = replyItems.size(); i < requestedOps.size(); i++) {
            // Re-enqueue every op that didn't execute in the request.
            toRetry.push_back(requestedOps[i]);
            // If we are logging the ops being re-enqueued, update 'opsStream'.
            if (logOps) {
                opsStream << (numOpsInStream++ > 0 ? ", " : "") << requestedOps[i].getId();
            }
        }

        LOGV2_DEBUG(
            10411404, 4, "re-enqueuing ops not completed by shard", "ops"_attr = opsStream.str());
    }

    return toRetry;
}

void WriteBatchResponseProcessor::recordTargetError(OperationContext* opCtx,
                                                    const WriteOp& op,
                                                    const Status& status) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));

    if (_nErrors == 0 || (!_cmdRef.getOrdered() && !inTransaction)) {
        processError(op, status, boost::none);
    }

    // If the write command is running in a transaction and there was a transient transaction
    // error, uassert so that the error is returned directly as a top level error, to allow the
    // client to retry.
    if (isTransientTxnError(status, inTransaction)) {
        uassertStatusOK(status);
    }
}

void WriteBatchResponseProcessor::recordErrorForRemainingOps(OperationContext* opCtx,
                                                             const Status& status) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    const size_t numOps = _cmdRef.getNumOps();

    for (size_t i = 0; i < numOps; ++i) {
        if (_nErrors > 0 && (_cmdRef.getOrdered() || inTransaction)) {
            break;
        }

        WriteOp op{WriteOpRef{_cmdRef, static_cast<int>(i)}};
        processError(op, status, boost::none);
    }
}

std::map<WriteOpId, BulkWriteReplyItem> WriteBatchResponseProcessor::finalizeRepliesForOps() {

    std::map<WriteOpId, BulkWriteReplyItem> aggregatedReplies;

    for (auto& [opId, opResult] : _results) {

        // If we have a single BulkWriteReplyItem with no attached shardId then there is no work to
        // do.
        if (std::holds_alternative<BulkWriteReplyItem>(opResult.replies)) {
            aggregatedReplies.emplace(opId, std::get<BulkWriteReplyItem>(opResult.replies));
            continue;
        }

        auto& replies = std::get<ReplyItemsByShard>(opResult.replies);
        tassert(10412306, "Expected at least one reply item", !replies.empty());

        // If we only have one reply item and it's not a retryable error, we just use that as the
        // reply. A single retryable error can be ignored as it means we have an unfinished
        // operation in the case we aborted for some reason.
        if (replies.size() == 1) {
            tassert(10412302,
                    "Expected a successful reply or non-retryable error in operation replies",
                    !write_op_helpers::isRetryErrCode(replies.begin()->second.getStatus().code()));
            aggregatedReplies.emplace(opId, replies.begin()->second);
            continue;
        }

        std::vector<BulkWriteReplyItem> successfulReplies;
        std::vector<BulkWriteReplyItem> errorReplies;

        // If we're here we have multiple reply items to combine.
        for (auto& [shardId, replyItem] : replies) {
            if (replyItem.getStatus().isOK()) {
                successfulReplies.push_back(replyItem);
            } else {
                tassert(
                    10412303,
                    "Expected a successful reply or non-retryable error in operation replies",
                    !write_op_helpers::isRetryErrCode(replies.begin()->second.getStatus().code()));

                errorReplies.push_back(replyItem);
            }
        }

        BulkWriteReplyItem reply;
        if (successfulReplies.size() == 0 && errorReplies.size() == 0) {
            continue;
        } else if (errorReplies.size() == 0) {
            reply = combineSuccessfulReplies(opId, successfulReplies);
        } else if (successfulReplies.size() == 0) {
            reply = combineErrorReplies(opId, errorReplies);
        } else {
            // We have a combination of errors and successes.
            auto successReply = combineSuccessfulReplies(opId, successfulReplies);

            reply = combineErrorReplies(opId, errorReplies);
            reply.setN(successReply.getN());
            reply.setNModified(successReply.getNModified());
            reply.setUpserted(successReply.getUpserted());
        }

        aggregatedReplies.emplace(opId, reply);
    }

    return aggregatedReplies;
}

WriteCommandResponse WriteBatchResponseProcessor::generateClientResponse(OperationContext* opCtx) {
    return _cmdRef.visitRequest(OverloadedVisitor{
        [&](const BatchedCommandRequest&) {
            return WriteCommandResponse{generateClientResponseForBatchedCommand()};
        },
        [&](const BulkWriteCommandRequest&) {
            return WriteCommandResponse{generateClientResponseForBulkWriteCommand(opCtx)};
        }});
}

BulkWriteCommandReply WriteBatchResponseProcessor::generateClientResponseForBulkWriteCommand(
    OperationContext* opCtx) {
    // Generate the list of reply items that should be returned to the client. For non-verbose bulk
    // write command requests, we always return an empty list of reply items. This matches the
    // behavior of ClusterBulkWriteCmd::Invocation::_populateCursorReply(). We call
    // 'finalizeRepliesForOps' to aggregate replies from different shards for a single op.
    std::map<WriteOpId, BulkWriteReplyItem> finalResults = finalizeRepliesForOps();

    std::vector<BulkWriteReplyItem> results;
    for (const auto& [id, item] : finalResults) {
        if (!_isNonVerbose) {
            results.push_back(item);
            // Set the Idx to be the one from the original client request.
            tassert(10347002,
                    fmt::format("expected id in reply ({}) to match id of operation from "
                                "original request ({})",
                                item.getIdx(),
                                id),
                    static_cast<WriteOpId>(item.getIdx()) == id);
        }
        _stats.incrementOpCounters(opCtx, _cmdRef.getOp(id));
    }

    bulk_write_exec::SummaryFields fields(
        _nErrors, _nInserted, _nMatched, _nModified, _nUpserted, _nDeleted);
    bulk_write_exec::BulkWriteReplyInfo info(
        std::move(results), std::move(fields), boost::none, getRetriedStmtIds());

    // Aggregate all the write concern errors from the shards.
    if (auto totalWcError = mergeWriteConcernErrors(_wcErrors); totalWcError) {
        info.wcErrors = BulkWriteWriteConcernError{totalWcError->toStatus().code(),
                                                   totalWcError->toStatus().reason()};
    }

    return populateCursorReply(
        opCtx, _cmdRef.getBulkWriteCommandRequest(), _originalCommand, std::move(info));
}

BatchedCommandResponse WriteBatchResponseProcessor::generateClientResponseForBatchedCommand() {
    BatchedCommandResponse resp;
    resp.setStatus(Status::OK());

    // For non-verbose batched command requests, we always return an OK response with n=0.
    // This matches the behavior of BatchWriteOp::buildClientResponse().
    if (_isNonVerbose) {
        return resp;
    }

    // We call 'finalizeRepliesForOps' to aggregate replies from different shards for a single op.
    std::map<WriteOpId, BulkWriteReplyItem> finalResults = finalizeRepliesForOps();

    for (const auto& [id, item] : finalResults) {
        auto status = item.getStatus();
        if (!status.isOK()) {
            resp.addToErrDetails(write_ops::WriteError(id, status));
        }

        // Verify that the id matches the one from the original client request.
        tassert(10605504,
                fmt::format(
                    "expected id in reply ({}) to match id of operation from original request ({})",
                    item.getIdx(),
                    id),
                static_cast<WriteOpId>(item.getIdx()) == id);

        // Handle propagating 'upsertedId' information.
        if (const auto& upserted = item.getUpserted(); upserted) {
            auto detail = std::make_unique<BatchedUpsertDetail>();

            detail->setIndex(id);

            BSONObjBuilder upsertedObjBuilder;
            upserted->serializeToBSON("_id", &upsertedObjBuilder);
            detail->setUpsertedID(upsertedObjBuilder.done());

            resp.addToUpsertDetails(detail.release());
        }
    }

    const int nValue = _nInserted + _nUpserted + _nMatched + _nDeleted;
    resp.setN(nValue);
    if (_cmdRef.isBatchWriteCommand() &&
        _cmdRef.getBatchedCommandRequest().getBatchType() ==
            BatchedCommandRequest::BatchType_Update &&
        _nModified >= 0) {
        resp.setNModified(_nModified);
    }
    resp.setRetriedStmtIds(getRetriedStmtIds());

    // Aggregate all the write concern errors from the shards.
    if (auto totalWcError = mergeWriteConcernErrors(_wcErrors); totalWcError) {
        resp.setWriteConcernError(new WriteConcernErrorDetail{totalWcError->toStatus()});
    }

    return resp;
}

}  // namespace mongo::unified_write_executor
