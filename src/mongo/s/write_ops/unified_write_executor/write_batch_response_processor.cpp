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
#include "mongo/db/global_catalog/router_role_api/collection_uuid_mismatch.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_server_params_gen.h"
#include "mongo/s/commands/query_cmd/populate_cursor.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/unified_write_executor/write_batch_executor.h"
#include "mongo/s/write_ops/write_op_helper.h"
#include "mongo/util/assert_util.h"

#include <variant>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::unified_write_executor {

static ErrorCodes::Error getStatusCode(const BulkWriteReplyItem& item) {
    return item.getStatus().code();
}

namespace {
template <typename ResultT>
void handleTransientTxnError(OperationContext* opCtx,
                             const ResultT& result,
                             boost::optional<HostAndPort> target = boost::none) {
    if (result.hasTransientTxnError()) {
        const auto& status = result.getTransientTxnError();

        uassertStatusOK(status.withContext(str::stream()
                                           << "Encountered error from "
                                           << (target ? target->toString() : "<unknown>")
                                           << " during a transaction"));
    }
}
}  // namespace

ProcessorResult WriteBatchResponseProcessor::onWriteBatchResponse(
    OperationContext* opCtx, RoutingContext& routingCtx, const WriteBatchResponse& response) {
    return visit(
        [&](const auto& responseData) -> ProcessorResult {
            return _onWriteBatchResponse(opCtx, routingCtx, responseData);
        },
        response);
}

ProcessorResult WriteBatchResponseProcessor::_onWriteBatchResponse(
    OperationContext* opCtx, RoutingContext& routingCtx, const EmptyBatchResponse& response) {
    return ProcessorResult{};
}

ProcessorResult WriteBatchResponseProcessor::_onWriteBatchResponse(
    OperationContext* opCtx, RoutingContext& routingCtx, const SimpleWriteBatchResponse& response) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));

    ProcessorResult result;

    for (const auto& [shardId, shardResponse] : response) {
        auto shardResult = onShardResponse(opCtx, routingCtx, shardId, shardResponse);
        result.combine(std::move(shardResult));

        // If this command is running in a transaction and an error occurs, stop processing
        // immediately and return a ProcessorResult for what we've processed so far.
        if (_nErrors > 0 && inTransaction) {
            break;
        }
    }

    removeFailedOpsFromOpsToRetry(result);
    return result;
}

ProcessorResult WriteBatchResponseProcessor::_onWriteBatchResponse(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    const NoRetryWriteBatchResponse& response) {
    // TODO SERVER-104122 Support for 'WouldChangeOwningShard' writes.
    // TODO SERVER-105762 Add support for errorsOnly: true.
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    const auto& swRes = response.swReply;
    const auto& op = response.op;

    // Process write concern error (if any).
    if (response.wce) {
        _wcErrors.push_back(ShardWCError{std::move(*response.wce)});
    }

    if (response.isError()) {
        // TODO SERVER-105303 Handle interruption/shutdown.
        // TODO SERVER-104122 Support for 'WouldChangeOwningShard' writes.
        LOGV2_DEBUG(10896500,
                    4,
                    "Cluster write op executing in internal transaction failed with error",
                    "error"_attr = redact(swRes.getStatus()));

        handleTransientTxnError(opCtx, response);

        // Process the local or top-level error for the batch.
        processErrorForBatch(opCtx, std::vector{op}, swRes.getStatus(), boost::none);

        if (inTransaction) {
            LOGV2_DEBUG(10413100,
                        4,
                        "Aborting write command due to error in a transaction",
                        "error"_attr = redact(swRes.getStatus()));
        }

        return ProcessorResult{};
    }

    visit(OverloadedVisitor(
              [&](const BulkWriteCommandReply& parsedReply) {
                  const auto& replyItems = parsedReply.getCursor().getFirstBatch();

                  tassert(10378000,
                          "Unexpected reply for NoRetryWriteBatchResponse",
                          replyItems.size() == 1);

                  const auto& replyItem = replyItems.front();
                  tassert(10378001,
                          fmt::format("reply with invalid opId {} when command only had 1 op",
                                      replyItem.getIdx()),
                          static_cast<WriteOpId>(replyItem.getIdx()) == 0);

                  // Process the reply item or error.
                  if (replyItem.getStatus().isOK()) {
                      processReplyItem(opCtx, op, std::move(replyItem), boost::none);
                  } else {
                      tassert(11222400,
                              "Unexpected retryable error reply from NoRetryWriteBatchResponse",
                              !write_op_helpers::isRetryErrCode(replyItem.getStatus().code()));

                      processError(opCtx, op, replyItem.getStatus(), boost::none);
                  }

                  processCountersAndRetriedStmtIds(parsedReply);
              },
              [&](const write_ops::FindAndModifyCommandReply& parsedReply) {
                  processFindAndModifyReply(opCtx, op, parsedReply);
              }),
          response.getReply());

    // Batch types that produce NoRetryWriteBatchResponse are executed using a mechanism
    // that deals with stale errors and retrying internally, so 'opsToRetry' will always
    // be empty.
    //
    // Likewise, batch types that produce NoRetryWriteBatchResponse cannot perform
    // inserts, so 'collsToCreate' will always be empty as well.
    return ProcessorResult{};
}

ProcessorResult WriteBatchResponseProcessor::handleRetryableError(OperationContext* opCtx,
                                                                  RoutingContext& routingCtx,
                                                                  boost::optional<WriteOp> op,
                                                                  const Status& status) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    const auto itemCode = status.code();

    ProcessorResult result;
    auto& collsToCreate = result.collsToCreate;

    if (itemCode == ErrorCodes::CannotImplicitlyCreateCollection) {
        if (!inTransaction) {
            // Stage the collection to be created if it was found to not exist.
            auto info = status.extraInfo<CannotImplicitlyCreateCollectionInfo>();
            if (auto it = collsToCreate.find(info->getNss()); it == collsToCreate.cend()) {
                collsToCreate.emplace(info->getNss(), std::move(info));
            }
        }
    } else if (itemCode == ErrorCodes::StaleDbVersion ||
               ErrorCodes::isStaleShardVersionError(itemCode)) {
        if (itemCode == ErrorCodes::StaleDbVersion) {
            LOGV2_DEBUG(10411403, 4, "Noting stale database response", "status"_attr = status);
        } else {
            LOGV2_DEBUG(10346900, 4, "Noting stale config response", "status"_attr = status);
        }

        if (op) {
            routingCtx.onStaleError(status, op->getNss());
        } else {
            routingCtx.onStaleError(status);
        }
    } else if (itemCode == ErrorCodes::ShardCannotRefreshDueToLocksHeld) {
        LOGV2_DEBUG(10413104,
                    4,
                    "Noting shard cannot refresh due to locks held response",
                    "status"_attr = status);
    }

    if (op && !inTransaction) {
        result.opsToRetry.emplace_back(*op);
    }

    return result;
}

ProcessorResult WriteBatchResponseProcessor::handleRetryableErrorForBatch(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    const std::vector<WriteOp>& ops,
    const Status& status) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));

    auto result = handleRetryableError(opCtx, routingCtx, /*op*/ boost::none, status);

    if (!inTransaction) {
        result.opsToRetry = ops;
    }

    return result;
}

void WriteBatchResponseProcessor::removeFailedOpsFromOpsToRetry(ProcessorResult& result) {
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

ProcessorResult WriteBatchResponseProcessor::onShardResponse(OperationContext* opCtx,
                                                             RoutingContext& routingCtx,
                                                             const ShardId& shardId,
                                                             const ShardResponse& response) {
    const auto& ops = response.ops;
    const auto& hostAndPort = response.hostAndPort;
    const bool ordered = _cmdRef.getOrdered();
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));

    ProcessorResult result{};

    // Process write concern error (if any).
    if (response.wce) {
        _wcErrors.push_back(ShardWCError(shardId, *response.wce));
    }

    // Handle the case where the response wasn't parsed due to an error in a transaction.
    if (response.isEmpty()) {
        return result;
    }

    // Handle the case where the response indicated that a top-level error occurred.
    if (response.isError()) {
        const auto& status = response.getStatus();
        const bool isRetryableErr = write_op_helpers::isRetryErrCode(status.code());

        // If 'status' is a retryable error, handle the retryable error for the batch.
        if (isRetryableErr) {
            result = handleRetryableErrorForBatch(opCtx, routingCtx, ops, status);
        }

        handleTransientTxnError(opCtx, response, hostAndPort);

        if (!isRetryableErr || inTransaction) {
            // Process the local error for the batch.
            processErrorForBatch(opCtx, ops, status, shardId);

            if (inTransaction) {
                LOGV2_DEBUG(10413101,
                            4,
                            "Aborting write command due to error in a transaction",
                            "error"_attr = redact(status),
                            "shardId"_attr = shardId);
            }
        }

        return result;
    }

    visit(OverloadedVisitor(
              [&](const BulkWriteCommandReply& parsedReply) {
                  auto replyItems = exhaustCursorForReplyItems(opCtx, shardId, parsedReply);

                  result = processOpsInReplyItems(opCtx, routingCtx, shardId, ops, replyItems);

                  if (_nErrors == 0 || (!ordered && !inTransaction)) {
                      result.opsToRetry =
                          processOpsNotInReplyItems(ops, replyItems, std::move(result.opsToRetry));
                  }

                  // Process the counters and the list of retried stmtIds.
                  processCountersAndRetriedStmtIds(parsedReply);
              },
              [&](const write_ops::FindAndModifyCommandReply& parsedReply) {
                  tassert(11272108, "Expected single write op for findAndModify", ops.size() == 1);

                  const auto& op = ops.front();

                  processFindAndModifyReply(opCtx, op, std::move(parsedReply));
              }),
          response.getReply());

    return result;
}

void WriteBatchResponseProcessor::processFindAndModifyReply(
    OperationContext* opCtx, const WriteOp& op, write_ops::FindAndModifyCommandReply reply) {
    tassert(10394904, "Expected no previous findAndModify result", _results.empty());

    _results.emplace(op.getId(), WriteOpResults(std::move(reply), /*hasNonRetryableError*/ false));
    _numOkResponses++;
}

void WriteBatchResponseProcessor::addReplyToResults(WriteOpId opId,
                                                    BulkWriteReplyItem reply,
                                                    boost::optional<ShardId> shardId) {
    bool hasNonRetryableError =
        !reply.getStatus().isOK() && !write_op_helpers::isRetryErrCode(reply.getStatus().code());

    updateApproximateSize(reply);

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
}

void WriteBatchResponseProcessor::processReplyItem(OperationContext* opCtx,
                                                   const WriteOp& op,
                                                   BulkWriteReplyItem item,
                                                   boost::optional<ShardId> shardId) {
    // Set the "idx" field to the ID of 'op'.
    item.setIdx(op.getId());

    // If this is an update op, ensure the "nModified" field is set.
    if (op.getType() == WriteType::kUpdate && !item.getNModified()) {
        item.setNModified(0);
    }

    if (item.getStatus().isOK()) {
        _numOkResponses++;
    } else {
        _nErrors++;
    }

    addReplyToResults(op.getId(), std::move(item), shardId);
}

void WriteBatchResponseProcessor::processCountersAndRetriedStmtIds(
    const BulkWriteCommandReply& parsedReply) {
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
}

void WriteBatchResponseProcessor::processError(OperationContext* opCtx,
                                               const WriteOp& op,
                                               const Status& status,
                                               boost::optional<ShardId> shardId) {
    tassert(10896503, "Unexpectedly got an OK status", !status.isOK());

    if (_cmdRef.isFindAndModifyCommand()) {
        tassert(10394905, "Expected no previous findAndModify result", _results.empty());

        _results.emplace(op.getId(), WriteOpResults(status, /*hasNonRetryableError*/ true));
        _nErrors++;
    } else {
        processReplyItem(opCtx, op, BulkWriteReplyItem(op.getId(), status), shardId);
    }
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
            processError(opCtx, firstOp, status, shardId);
        }
    } else {
        // If the write command is unordered and not in a transaction, record an error for each
        // op in 'ops'.
        for (const auto& op : ops) {
            processError(opCtx, op, status, shardId);
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

ProcessorResult WriteBatchResponseProcessor::processOpsInReplyItems(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    const ShardId shardId,
    const std::vector<WriteOp>& ops,
    const std::vector<BulkWriteReplyItem>& replyItems) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    ProcessorResult result;

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
        }

        if (write_op_helpers::isRetryErrCode(status.code())) {
            result.combine(handleRetryableError(opCtx, routingCtx, op, item.getStatus()));

            if (!inTransaction) {
                // If we got a retryable error outside of a transaction we don't add it to the reply
                // items, since it will be retried later.
                continue;
            }
        }

        processReplyItem(opCtx, op, item, shardId);

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

void WriteBatchResponseProcessor::updateApproximateSize(const BulkWriteReplyItem& item) {
    // If the response is not an error and 'errorsOnly' is set, then we should not store this reply
    // and therefore shouldn't count the size.
    if (item.getOk() && _cmdRef.getErrorsOnly() && *(_cmdRef.getErrorsOnly())) {
        return;
    }

    _approximateSize += item.getApproximateSize();
}

void WriteBatchResponseProcessor::recordTargetErrors(OperationContext* opCtx,
                                                     const BatcherResult& batcherResult) {
    handleTransientTxnError(opCtx, batcherResult);

    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    for (const auto& [op, status] : batcherResult.opsWithErrors) {
        if (_nErrors > 0 && (_cmdRef.getOrdered() || inTransaction)) {
            break;
        }

        processError(opCtx, op, status, boost::none);
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
        processError(opCtx, op, status, boost::none);
    }
}

bool WriteBatchResponseProcessor::checkBulkWriteReplyMaxSize() {
    // Cannot exceed the reply size limit if we have no responses.
    if (_results.empty()) {
        return false;
    }

    // If we have exceeded the max reply size limit, we will record an error for the first
    // incomplete WriteOp.
    const WriteOpId nextHighestWriteOpId = _results.rbegin()->first + 1;

    if (_cmdRef.isBulkWriteCommand() &&
        _approximateSize >= gBulkWriteMaxRepliesSize.loadRelaxed() &&
        nextHighestWriteOpId < _cmdRef.getNumOps()) {
        BulkWriteReplyItem exceededMemLimitReplyItem(
            nextHighestWriteOpId,
            Status{ErrorCodes::ExceededMemoryLimit,
                   fmt::format("BulkWrite response size exceeded limit ({} bytes)",
                               _approximateSize)});
        exceededMemLimitReplyItem.setOk(0.0);
        exceededMemLimitReplyItem.setN(0);
        exceededMemLimitReplyItem.setNModified(boost::none);

        _nErrors++;
        addReplyToResults(nextHighestWriteOpId, std::move(exceededMemLimitReplyItem), boost::none);
        return true;
    }
    return false;
}

std::map<WriteOpId, BulkWriteReplyItem> WriteBatchResponseProcessor::finalizeRepliesForOps(
    OperationContext* opCtx) {

    std::map<WriteOpId, BulkWriteReplyItem> aggregatedReplies;
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));

    for (auto& [opId, opResult] : _results) {
        // If we have a single BulkWriteReplyItem with no attached shardId then there is no work to
        // do.
        if (std::holds_alternative<BulkWriteReplyItem>(opResult.replies)) {
            aggregatedReplies.emplace(opId, std::get<BulkWriteReplyItem>(opResult.replies));
            continue;
        }

        auto& replies = std::get<ReplyItemsByShard>(opResult.replies);
        tassert(10412306, "Expected at least one reply item", !replies.empty());

        // If we only have one reply item, we just use that as the
        // reply.
        if (replies.size() == 1) {
            auto singleReply = replies.begin()->second;
            tassert(
                10412302,
                str::stream() << "Expected a successful reply or non-retryable error in operation "
                                 "replies outside of a transaction, but got"
                              << redact(singleReply.getStatus()),
                inTransaction || !write_op_helpers::isRetryErrCode(singleReply.getStatus().code()));
            aggregatedReplies.emplace(opId, singleReply);
            continue;
        }

        std::vector<BulkWriteReplyItem> successfulReplies;
        std::vector<BulkWriteReplyItem> errorReplies;

        // If we're here we have multiple reply items to combine.
        for (auto& [shardId, replyItem] : replies) {
            if (replyItem.getStatus().isOK()) {
                successfulReplies.push_back(replyItem);
            } else {
                tassert(10412303,
                        str::stream()
                            << "Expected a successful reply or non-retryable error in operation "
                               "replies outside of a transaction, but got"
                            << redact(replyItem.getStatus()),
                        inTransaction ||
                            !write_op_helpers::isRetryErrCode(replyItem.getStatus().code()));
                errorReplies.push_back(replyItem);
            }
        }

        BulkWriteReplyItem reply;
        if (successfulReplies.empty() && errorReplies.empty()) {
            continue;
        } else if (errorReplies.empty()) {
            reply = combineSuccessfulReplies(opId, successfulReplies);
        } else if (successfulReplies.empty()) {
            reply = combineErrorReplies(opId, errorReplies);
        } else {
            // We have a combination of errors and successes.
            auto successReply = combineSuccessfulReplies(opId, successfulReplies);

            auto errorReply = combineErrorReplies(opId, errorReplies);

            // There are errors that are safe to ignore if they were correctly applied to other
            // shards and we're using ShardVersion::IGNORED. They are safe to ignore as they can be
            // interpreted as no-ops if the shard response had been instead a successful result
            // since they wouldn't have modified any data. As a result, we can swallow the errors
            // and treat them as a successful operation.
            const bool canIgnoreErrors = write_op_helpers::shouldTargetAllShardsSVIgnored(
                                             inTransaction, _cmdRef.getOp(opId).getMulti()) &&
                write_op_helpers::isSafeToIgnoreErrorInPartiallyAppliedOp(errorReply.getStatus());
            if (canIgnoreErrors) {
                reply = std::move(successReply);
            } else {
                reply = std::move(errorReply);
                reply.setN(successReply.getN());
                reply.setNModified(successReply.getNModified());
                reply.setUpserted(successReply.getUpserted());
            }
        }

        aggregatedReplies.emplace(opId, reply);
    }

    return aggregatedReplies;
}

WriteCommandResponse WriteBatchResponseProcessor::generateClientResponse(OperationContext* opCtx) {
    return _cmdRef.visitRequest(OverloadedVisitor{
        [&](const BatchedCommandRequest&) {
            return WriteCommandResponse{generateClientResponseForBatchedCommand(opCtx)};
        },
        [&](const BulkWriteCommandRequest&) {
            return WriteCommandResponse{generateClientResponseForBulkWriteCommand(opCtx)};
        },
        [&](const write_ops::FindAndModifyCommandRequest&) {
            return WriteCommandResponse{generateClientResponseForFindAndModifyCommand()};
        }});
}

BulkWriteCommandReply WriteBatchResponseProcessor::generateClientResponseForBulkWriteCommand(
    OperationContext* opCtx) {
    // Generate the list of reply items that should be returned to the client. For non-verbose bulk
    // write command requests, we always return an empty list of reply items. This matches the
    // behavior of populateCursorReply(). We call 'finalizeRepliesForOps' to aggregate replies from
    // different shards for a single op.
    std::map<WriteOpId, BulkWriteReplyItem> finalResults = finalizeRepliesForOps(opCtx);

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

BatchedCommandResponse WriteBatchResponseProcessor::generateClientResponseForBatchedCommand(
    OperationContext* opCtx) {
    BatchedCommandResponse resp;
    resp.setStatus(Status::OK());

    // For non-verbose batched command requests, we always return an OK response with n=0.
    // This matches the behavior of BatchWriteOp::buildClientResponse().
    if (_isNonVerbose) {
        return resp;
    }

    // We call 'finalizeRepliesForOps' to aggregate replies from different shards for a single op.
    std::map<WriteOpId, BulkWriteReplyItem> finalResults = finalizeRepliesForOps(opCtx);

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
            BatchedCommandRequest::BatchType_Update) {
        resp.setNModified(_nModified);
    }
    resp.setRetriedStmtIds(getRetriedStmtIds());

    // Aggregate all the write concern errors from the shards.
    if (auto totalWcError = mergeWriteConcernErrors(_wcErrors); totalWcError) {
        resp.setWriteConcernError(new WriteConcernErrorDetail{totalWcError->toStatus()});
    }

    return resp;
}

FindAndModifyCommandResponse
WriteBatchResponseProcessor::generateClientResponseForFindAndModifyCommand() {
    tassert(10394906,
            "Expected a populated findAndModify reply",
            _results.size() == 1 &&
                std::holds_alternative<StatusWith<write_ops::FindAndModifyCommandReply>>(
                    _results.begin()->second.replies));

    auto reply = std::get<StatusWith<write_ops::FindAndModifyCommandReply>>(
        std::move(_results.begin()->second.replies));

    boost::optional<WriteConcernErrorDetail> wce = boost::none;
    if (auto totalWcError = mergeWriteConcernErrors(_wcErrors); totalWcError) {
        wce = WriteConcernErrorDetail{totalWcError->toStatus()};
    }

    return {reply, wce};
}

}  // namespace mongo::unified_write_executor
