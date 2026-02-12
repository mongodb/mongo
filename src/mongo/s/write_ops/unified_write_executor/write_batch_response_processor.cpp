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
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_server_params_gen.h"
#include "mongo/db/router_role/collection_uuid_mismatch.h"
#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/stats/counters.h"
#include "mongo/s/commands/query_cmd/populate_cursor.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/unified_write_executor/write_batch_executor.h"
#include "mongo/s/write_ops/write_op.h"
#include "mongo/s/write_ops/write_op_helper.h"
#include "mongo/util/assert_util.h"

#include <variant>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::unified_write_executor {

using ItemVariant = WriteBatchResponseProcessor::ItemVariant;
using Unexecuted = WriteBatchResponseProcessor::Unexecuted;
using SucceededWithoutItem = WriteBatchResponseProcessor::SucceededWithoutItem;
using FindAndModifyReplyItem = WriteBatchResponseProcessor::FindAndModifyReplyItem;
using ItemsByOpMap = WriteBatchResponseProcessor::ItemsByOpMap;
using ShardResult = WriteBatchResponseProcessor::ShardResult;

namespace {
// This function returns the error code for a given ItemVariant ('itemVar'). If 'itemVar' is
// Unexecuted, this function will return boost::none.
boost::optional<ErrorCodes::Error> getErrorCode(const ItemVariant& itemVar) {
    using RetT = boost::optional<ErrorCodes::Error>;
    return visit(
        OverloadedVisitor(
            [&](const Unexecuted&) -> RetT { return boost::none; },
            [&](const SucceededWithoutItem&) -> RetT { return ErrorCodes::OK; },
            [&](const BulkWriteReplyItem& item) -> RetT { return item.getStatus().code(); },
            [&](const FindAndModifyReplyItem& item) -> RetT {
                return item.swReply.getStatus().code();
            }),
        itemVar);
}

bool isOKItem(const ItemVariant& itemVar) {
    auto code = getErrorCode(itemVar);
    return code && *code == ErrorCodes::OK;
}

bool isRetryableError(const ItemVariant& itemVar) {
    auto code = getErrorCode(itemVar);
    return code && write_op_helpers::isRetryErrCode(*code);
}

bool isNonRetryableError(const ItemVariant& itemVar) {
    auto code = getErrorCode(itemVar);
    return code && *code != ErrorCodes::OK && !write_op_helpers::isRetryErrCode(*code);
}

template <typename ResultT>
void handleShutdownError(OperationContext* opCtx, const ResultT& result) {
    if (result.isShutdownError()) {
        uassertStatusOK(result.getStatus());
    }
}

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

std::shared_ptr<const CannotImplicitlyCreateCollectionInfo> getCannotImplicitlyCreateCollectionInfo(
    const Status& status) {
    if (status == ErrorCodes::CannotImplicitlyCreateCollection) {
        auto info = status.extraInfo<CannotImplicitlyCreateCollectionInfo>();
        tassert(11182204, "Expected to find CannotImplicitlyCreateCollectionInfo", info != nullptr);

        return info;
    }

    return {};
}

std::shared_ptr<const CannotImplicitlyCreateCollectionInfo> getCannotImplicitlyCreateCollectionInfo(
    const ItemVariant& itemVar) {
    using RetT = std::shared_ptr<const CannotImplicitlyCreateCollectionInfo>;
    return visit(OverloadedVisitor(
                     [&](const Unexecuted&) -> RetT { return {}; },
                     [&](const SucceededWithoutItem&) -> RetT { return {}; },
                     [&](const BulkWriteReplyItem& item) -> RetT {
                         return getCannotImplicitlyCreateCollectionInfo(item.getStatus());
                     },
                     [&](const FindAndModifyReplyItem& item) -> RetT {
                         return getCannotImplicitlyCreateCollectionInfo(item.swReply.getStatus());
                     }),
                 itemVar);
}

std::shared_ptr<const CollectionUUIDMismatchInfo> getCollectionUUIDMismatchInfo(
    const Status& status) {
    if (status == ErrorCodes::CollectionUUIDMismatch) {
        auto info = status.extraInfo<CollectionUUIDMismatchInfo>();
        tassert(11273500, "Expected to find CollectionUUIDMismatchInfo", info != nullptr);

        return info;
    }

    return {};
}

// Helper function that prints the contents of 'opsToRetry' to the log if appropriate.
void logOpsToRetry(const std::vector<WriteOp>& opsToRetry) {
    if (!opsToRetry.empty() &&
        shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, logv2::LogSeverity::Debug(4))) {
        std::stringstream opsStream;
        size_t numOpsInStream = 0;

        for (const auto& op : opsToRetry) {
            opsStream << (numOpsInStream++ > 0 ? ", " : "") << getWriteOpId(op);
        }

        LOGV2_DEBUG(
            10411404, 4, "re-enqueuing ops that didn't complete", "ops"_attr = opsStream.str());
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
    const bool ordered = _cmdRef.getOrdered();
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    auto isNonRetryableErrorForPair = [](auto&& p) {
        return isNonRetryableError(p.second);
    };

    std::vector<std::pair<ShardId, ShardResult>> shardResults;
    shardResults.reserve(response.shardResponses.size());

    // For each ShardResponse, call onShardResponse() and store the result into 'shardResults'.
    for (const auto& [shardId, shardResponse] : response.shardResponses) {
        shardResults.emplace_back(shardId,
                                  onShardResponse(opCtx, routingCtx, shardId, shardResponse));
    }

    // Organize the items from the shard responses by op.
    auto itemsByOp = groupItemsByOp(opCtx, shardResults, response.opsUsingSVIgnored);

    // For "RetryableWriteWithId" simple write batches, a different retry strategy is used. If
    // 'inTransaction' is false -AND- the batch contains a retryable error -AND- there wasn't
    // a non-retryable error, then all of the ops in the batch are retried regardless of what
    // their execution status was.
    //
    // This is the key difference between "RetryableWriteWithId" simple write batches and regular
    // simple write batches.
    if (response.isRetryableWriteWithId && !inTransaction) {
        bool hasRetryableErr = false;
        bool hasNonRetryableErr = false;
        for (auto& [op, items] : itemsByOp) {
            for (auto& [_, itemVar] : items) {
                hasRetryableErr |= isRetryableError(itemVar);
                hasNonRetryableErr |= isNonRetryableError(itemVar);
            }
        }

        if (hasRetryableErr && !(ordered && hasNonRetryableErr)) {
            std::set<WriteOp> toRetry;
            CollectionsToCreate collsToCreate;

            // For each 'op', queue 'op' for retry and also increment counters as appropriate.
            for (auto& [op, items] : itemsByOp) {
                for (const auto& [shardId, itemVar] : items) {
                    queueOpForRetry(op, toRetry);
                    queueCreateCollectionIfNeeded(itemVar, collsToCreate);
                }

                if (getWriteOpType(op) == kUpdate) {
                    getQueryCounters(opCtx).updateOneWithoutShardKeyWithIdRetryCount.increment(1);
                } else if (getWriteOpType(op) == kDelete) {
                    getQueryCounters(opCtx).deleteOneWithoutShardKeyWithIdRetryCount.increment(1);
                }
            }

            ProcessorResult result;
            result.opsToRetry.insert(result.opsToRetry.end(), toRetry.begin(), toRetry.end());
            result.collsToCreate = std::move(collsToCreate);

            // Print the contents of 'opsToRetry' to the log if appropriate.
            logOpsToRetry(result.opsToRetry);

            return result;
        }
    }

    // Update the counters (excluding _nErrors), update the list of retried stmtIds, and process
    // the write concern error (if any).
    for (const auto& [shardId, shardResult] : shardResults) {
        if (shardResult.batchWriteReply) {
            processCountersAndRetriedStmtIds(*shardResult.batchWriteReply);
        } else if (shardResult.bulkWriteReply) {
            processCountersAndRetriedStmtIds(*shardResult.bulkWriteReply);
        }
        if (shardResult.wce) {
            _wcErrors.push_back(ShardWCError(shardId, *shardResult.wce));
        }
    }

    std::set<WriteOp> toRetry;
    CollectionsToCreate collsToCreate;
    absl::flat_hash_map<WriteOpId, std::set<ShardId>> successfulShardSet;
    bool continueProcessing = true;

    // Process the results for each op that was part of this batch.
    for (auto& [op, items] : itemsByOp) {
        tassert(11182201, "Expected op to have at least one item", !items.empty());

        const bool shouldRetryOnUnexecutedOrRetryableError =
            !inTransaction && std::none_of(items.begin(), items.end(), isNonRetryableErrorForPair);

        // Process all of the reply items that correspond to 'op'.
        for (const auto& [shardId, itemVar] : items) {
            // Visit the item. This visitor returns false if the outer loop should stop after the
            // current iteration.
            auto visitItem = OverloadedVisitor(
                [&](const Unexecuted&) {
                    // If 'shouldRetryOnUnexecutedOrRetryableError' is true, then queue 'op' to
                    // be retried.
                    if (shouldRetryOnUnexecutedOrRetryableError) {
                        queueOpForRetry(op, toRetry);
                    }
                    return true;
                },
                [&](const SucceededWithoutItem&) {
                    // Add a successful entry with no BulkWriteReplyItem to _results, and add
                    // 'shardId' to 'successfulShardSet'.
                    successfulShardSet[getWriteOpId(op)].insert(shardId);
                    recordResult(opCtx, op, boost::none);
                    return true;
                },
                [&](const BulkWriteReplyItem& item) {
                    const auto& status = item.getStatus();
                    // If 'item' has an OK status, add 'shardId' to 'successfulShardSet'.
                    if (status.isOK()) {
                        successfulShardSet[getWriteOpId(op)].insert(shardId);
                    }

                    if (!write_op_helpers::isRetryErrCode(status.code()) || inTransaction) {
                        // If 'item' is a not a retryable error, or if the command is running in a
                        // transaction, record the result.
                        recordResult(opCtx, op, std::move(item));

                        // If we recorded an error and the write command is ordered or running in
                        // a transaction, then return false to stop processing.
                        if (_nErrors > 0 && (ordered || inTransaction)) {
                            return false;
                        }
                    } else if (shouldRetryOnUnexecutedOrRetryableError) {
                        // If the command isn't running in a transaction and 'item' is a retryable
                        // error and 'shouldRetryOnUnexecutedOrRetryableError' is true, then queue
                        // 'op' to be retried.
                        queueOpForRetry(op, toRetry);
                        queueCreateCollectionIfNeeded(status, collsToCreate);
                    }
                    return true;
                },
                [&](const FindAndModifyReplyItem& item) {
                    const auto& status = item.swReply.getStatus();
                    if (!write_op_helpers::isRetryErrCode(status.code()) || inTransaction) {
                        // Add 'item' to _results.
                        recordResult(opCtx, op, std::move(item));
                        // If we recorded an error and the write command is ordered or running in
                        // a transaction, then return false to stop processing.
                        if (_nErrors > 0 && (ordered || inTransaction)) {
                            return false;
                        }
                    } else if (shouldRetryOnUnexecutedOrRetryableError) {
                        // If the command isn't running in a transaction and 'item' is a retryable
                        // error and 'shouldRetryOnUnexecutedOrRetryableError' is true, then queue
                        // 'op' to be retried.
                        queueOpForRetry(op, toRetry);
                        queueCreateCollectionIfNeeded(status, collsToCreate);
                    }
                    return true;
                });

            continueProcessing &= visit(std::move(visitItem), itemVar);
        }

        // If processing should not continue, break out of the outer loop.
        if (!continueProcessing) {
            break;
        }
    }

    ProcessorResult result;
    result.opsToRetry.insert(result.opsToRetry.end(), toRetry.begin(), toRetry.end());
    result.collsToCreate = std::move(collsToCreate);
    result.successfulShardSet = std::move(successfulShardSet);

    // For "RetryableWriteWithId" batches, if the "hangAfterCompletingWriteWithoutShardKeyWithId"
    // failpoint is set, call pauseWhileSet().
    if (response.isRetryableWriteWithId) {
        auto& fp = getHangAfterCompletingWriteWithoutShardKeyWithIdFailPoint();
        if (MONGO_unlikely(fp.shouldFail())) {
            fp.pauseWhileSet();
        }
    }

    // Print the contents of 'opsToRetry' to the log if appropriate.
    logOpsToRetry(result.opsToRetry);

    return result;
}

ProcessorResult WriteBatchResponseProcessor::_onWriteBatchResponse(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    const NoRetryWriteBatchResponse& response) {
    const auto& op = response.getOp();

    // Process write concern error (if any).
    if (response.getWriteConcernError()) {
        _wcErrors.push_back(ShardWCError{*response.getWriteConcernError()});
    }

    tassert(11182202, "Expected non-empty response", !response.isEmpty());

    if (response.isError()) {
        const auto& status = response.getStatus();

        LOGV2_DEBUG(10896500,
                    4,
                    "Cluster write op executing in internal transaction failed with error",
                    "error"_attr = redact(status));

        handleShutdownError(opCtx, response);

        handleTransientTxnError(opCtx, response);

        recordError(opCtx, op, status);

        return ProcessorResult{};
    }

    // Helper lambda for processing BulkWriteReplyItems.
    auto processItems = [&](const std::vector<BulkWriteReplyItem>& replyItems) {
        // Get the BulkWriteReplyItem (if there is one) and store it in 'item'.
        boost::optional<BulkWriteReplyItem> item;

        if (!replyItems.empty()) {
            item = replyItems.front();

            tassert(10378001,
                    fmt::format("reply with invalid opId {} when command only had 1 op",
                                item->getIdx()),
                    item->getIdx() == 0);

            tassert(11222400,
                    "Unexpected retryable error reply from NoRetryWriteBatchResponse",
                    item->getStatus().isOK() ||
                        !write_op_helpers::isRetryErrCode(item->getStatus().code()));

            // Set the "idx" field to the ID of 'op'.
            item->setIdx(getWriteOpId(op));
        }

        // Add 'item' to _results.
        recordResult(opCtx, op, std::move(item));
    };

    visit(OverloadedVisitor(
              [&](const BatchWriteCommandReply& parsedReply) {
                  const std::vector<BulkWriteReplyItem>& replyItems = parsedReply.items;
                  tassert(11468111,
                          "Unexpected reply for NoRetryWriteBatchResponse",
                          replyItems.size() <= 1);

                  // Process the BulkWriteReplyItems in 'parsedReply'.
                  processItems(replyItems);
                  // Update the counters (excluding _nErrors) and the list of retried stmtIds.
                  processCountersAndRetriedStmtIds(parsedReply);
              },
              [&](const BulkWriteCommandReply& parsedReply) {
                  const bool errorsOnly = _cmdRef.getErrorsOnly().value_or(false);
                  const std::vector<BulkWriteReplyItem>& replyItems =
                      parsedReply.getCursor().getFirstBatch();
                  tassert(10378000,
                          "Unexpected reply for NoRetryWriteBatchResponse",
                          parsedReply.getCursor().getId() == 0 && replyItems.size() <= 1 &&
                              (errorsOnly || replyItems.size() == 1));

                  // Process the BulkWriteReplyItems in 'parsedReply'.
                  processItems(replyItems);
                  // Update the counters (excluding _nErrors) and the list of retried stmtIds.
                  processCountersAndRetriedStmtIds(parsedReply);
              },
              [&](const write_ops::FindAndModifyCommandReply& parsedReply) {
                  recordResult(
                      opCtx, op, FindAndModifyReplyItem{parsedReply, response.getShardId()});
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

void WriteBatchResponseProcessor::noteRetryableError(OperationContext* opCtx,
                                                     RoutingContext& routingCtx,
                                                     const Status& status) {
    if (status == ErrorCodes::CannotImplicitlyCreateCollection) {
        LOGV2_DEBUG(11182203,
                    4,
                    "Noting cannotImplicitlyCreateCollection response",
                    "status"_attr = status);
    } else if (status == ErrorCodes::StaleDbVersion ||
               ErrorCodes::isStaleShardVersionError(status.code())) {
        if (status == ErrorCodes::StaleDbVersion) {
            LOGV2_DEBUG(10411403, 4, "Noting stale database response", "status"_attr = status);
        } else {
            LOGV2_DEBUG(10346900, 4, "Noting stale config response", "status"_attr = status);
        }
        routingCtx.onStaleError(status);
    } else if (status == ErrorCodes::ShardCannotRefreshDueToLocksHeld) {
        LOGV2_DEBUG(10413104,
                    4,
                    "Noting shard cannot refresh due to locks held response",
                    "status"_attr = status);
    }
}

void WriteBatchResponseProcessor::queueOpForRetry(const WriteOp& op,
                                                  std::set<WriteOp>& toRetry) const {
    toRetry.emplace(op);
}

void WriteBatchResponseProcessor::queueCreateCollectionIfNeeded(
    std::shared_ptr<const CannotImplicitlyCreateCollectionInfo> info,
    CollectionsToCreate& collsToCreate) {
    // If 'info' is not null, add it to 'collsToCreate'.
    if (info) {
        auto nss = info->getNss();
        collsToCreate.emplace(std::move(nss), std::move(info));
    }
}

void WriteBatchResponseProcessor::queueCreateCollectionIfNeeded(
    const Status& status, CollectionsToCreate& collsToCreate) {
    return queueCreateCollectionIfNeeded(getCannotImplicitlyCreateCollectionInfo(status),
                                         collsToCreate);
}

void WriteBatchResponseProcessor::queueCreateCollectionIfNeeded(
    const ItemVariant& itemVar, CollectionsToCreate& collsToCreate) {
    return queueCreateCollectionIfNeeded(getCannotImplicitlyCreateCollectionInfo(itemVar),
                                         collsToCreate);
}

ShardResult WriteBatchResponseProcessor::onShardResponse(OperationContext* opCtx,
                                                         RoutingContext& routingCtx,
                                                         const ShardId& shardId,
                                                         const ShardResponse& response) {
    const auto& ops = response.getOps();
    const auto& hostAndPort = response.getHostAndPort();
    const bool orderedOrInTxn = _cmdRef.getOrdered() || TransactionRouter::get(opCtx);

    ShardResult result;
    result.wce = response.getWriteConcernError();
    result.items.reserve(ops.size());

    // Handle the case where the response wasn't parsed due to an error in a transaction.
    if (response.isEmpty()) {
        // Generate an "unexecuted" item for each op in 'ops'.
        for (const auto& op : ops) {
            result.items.emplace_back(op, Unexecuted{});
        }

        return result;
    }

    // Handle the case where the response indicated that a top-level error occurred.
    if (response.isError()) {
        // If 'status' is a stale error, inform 'routingCtx' if needed.
        const auto& status = response.getStatus();
        const bool isRetryableErr = write_op_helpers::isRetryErrCode(status.code());

        if (isRetryableErr) {
            noteRetryableError(opCtx, routingCtx, status);
        }

        handleShutdownError(opCtx, response);

        handleTransientTxnError(opCtx, response, hostAndPort);

        // Generate item errors for individual ops as appropriate.
        if (orderedOrInTxn || isRetryableErr) {
            if (!ops.empty()) {
                const auto& firstOp = *std::min_element(ops.begin(), ops.end());
                for (const auto& op : ops) {
                    if (op == firstOp) {
                        result.items.emplace_back(op, makeErrorItem(op, status, shardId));
                    } else {
                        result.items.emplace_back(op, Unexecuted{});
                    }
                }
            }
        } else {
            for (const auto& op : ops) {
                result.items.emplace_back(op, makeErrorItem(op, status, shardId));
            }
        }

        return result;
    }

    visit(OverloadedVisitor(
              [&](const BatchWriteCommandReply& parsedReply) -> void {
                  result.batchWriteReply.emplace(parsedReply);

                  retrieveBatchWriteReplyItems(opCtx, routingCtx, shardId, ops, result);
              },
              [&](const BulkWriteCommandReply& parsedReply) {
                  const bool errorsOnly = _cmdRef.getErrorsOnly().value_or(false);
                  result.bulkWriteReply = parsedReply;

                  retrieveBulkWriteReplyItems(opCtx, routingCtx, shardId, ops, errorsOnly, result);
              },
              [&](const write_ops::FindAndModifyCommandReply& parsedReply) {
                  tassert(11272108, "Expected single write op for findAndModify", ops.size() == 1);
                  const auto& op = ops.front();

                  result.items.emplace_back(op, FindAndModifyReplyItem{parsedReply, shardId});
              }),
          response.getReply());

    return result;
}

void WriteBatchResponseProcessor::recordResult(OperationContext* opCtx,
                                               const WriteOp& op,
                                               boost::optional<BulkWriteReplyItem> item) {
    const bool isOK = item ? item->getStatus().isOK() : true;

    auto [it, _] = _results.try_emplace(getWriteOpId(op), BulkWriteOpResults{});
    auto* opResults = get_if<BulkWriteOpResults>(&it->second);
    tassert(11182205, "Expected to find BulkWriteOpResults", opResults != nullptr);

    if (isOK) {
        ++_numOkItemsProcessed;
    } else {
        // Increment '_nErrors' only if this is the first error reply we have seen for this op.
        // This is important so that we avoid over-counting the number of ops that had errors.
        if (!opResults->hasError) {
            // If this is the first error and we're running in a transaction, log the error.
            if (_nErrors == 0 && TransactionRouter::get(opCtx)) {
                LOGV2_DEBUG(10413103,
                            2,
                            "Aborting write command due to error in transaction",
                            "error"_attr = redact(item->getStatus()));
            }

            ++_nErrors;
        }
    }

    opResults->hasSuccess |= isOK;
    opResults->hasError |= !isOK;

    if (item) {
        updateApproximateSize(*item);
        opResults->items.emplace_back(std::move(*item));
    }
}

void WriteBatchResponseProcessor::recordResult(OperationContext* opCtx,
                                               const WriteOp& op,
                                               FindAndModifyReplyItem item) {
    const bool isOK = item.swReply.getStatus().isOK();
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));

    if (isOK) {
        ++_numOkItemsProcessed;
    } else {
        // If this is the first error and we're running in a transaction, log the error.
        if (_nErrors == 0 && inTransaction) {
            LOGV2_DEBUG(10413100,
                        2,
                        "Aborting write command due to error in transaction",
                        "error"_attr = redact(item.swReply.getStatus()));
        }

        ++_nErrors;
    }

    auto [_, inserted] = _results.try_emplace(getWriteOpId(op), std::move(item));
    tassert(11182206, "Expected no previous findAndModify result for op", inserted);
}

void WriteBatchResponseProcessor::recordError(OperationContext* opCtx,
                                              const WriteOp& op,
                                              const Status& status) {
    tassert(11182207, "Expected non-OK status", !status.isOK());

    if (op.isFindAndModify()) {
        auto item = FindAndModifyReplyItem{status};

        recordResult(opCtx, op, std::move(item));
    } else {
        auto item = BulkWriteReplyItem(getWriteOpId(op), status);
        if (getWriteOpType(op) == WriteType::kUpdate) {
            item.setNModified(0);
        }

        recordResult(opCtx, op, std::move(item));
    }
}

ItemVariant WriteBatchResponseProcessor::makeErrorItem(const WriteOp& op,
                                                       const Status& status,
                                                       const ShardId& shardId) const {
    tassert(11182208, "Expected non-OK status", !status.isOK());

    if (op.isFindAndModify()) {
        return ItemVariant{FindAndModifyReplyItem{status, shardId}};
    } else {
        auto item = BulkWriteReplyItem(getWriteOpId(op), status);
        if (getWriteOpType(op) == WriteType::kUpdate) {
            item.setNModified(0);
        }
        return ItemVariant{std::move(item)};
    }
}

ItemsByOpMap WriteBatchResponseProcessor::groupItemsByOp(
    OperationContext* opCtx,
    const std::vector<std::pair<ShardId, ShardResult>>& shardResults,
    const absl::flat_hash_set<WriteOpId>& opsUsingSVIgnored) const {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    auto isOKItemForPair = [](auto&& p) {
        return isOKItem(p.second);
    };

    auto isCollUUIDMismatchWithoutActualNamespace = [&](const ItemVariant& itemVar) -> bool {
        if (const auto* bwItem = get_if<BulkWriteReplyItem>(&itemVar)) {
            return write_op_helpers::isCollUUIDMismatchWithoutActualNamespace(bwItem->getStatus());
        }
        return false;
    };

    ItemsByOpMap itemMap;
    absl::flat_hash_set<WriteOp> opsToRevisit;

    // Organize the items by op and store them into 'itemMap'.
    for (const auto& [shardId, shardResult] : shardResults) {
        for (const auto& [op, itemVar] : shardResult.items) {
            const auto opId = getWriteOpId(op);
            const bool opUsesSVIgnored = !inTransaction && opsUsingSVIgnored.count(opId);

            // For ops that use shardVersion IGNORED and are broadcast to all shards, by design we
            // ignore CollectionUUIDMismatch errors that don't have an actual namespace if the op
            // was successful on other shards.
            //
            // We implement this special behavior by adding the op to 'opsToRevisit' and then
            // revisiting the op during a second pass (after this loop finishes).
            if (opUsesSVIgnored && isCollUUIDMismatchWithoutActualNamespace(itemVar)) {
                opsToRevisit.emplace(op);
            }

            // Store 'itemVar' into 'itemMap'.
            auto [it, _] = itemMap.try_emplace(op);
            it->second.emplace_back(shardId, itemVar);
        }
    }

    // Visit the ops in 'opsToRevisit' and ignore CollectionUUIDMismatch errors for these ops
    // as appropriate.
    for (auto& op : opsToRevisit) {
        auto& items = itemMap[op];
        auto opId = getWriteOpId(op);

        if (opHasSuccess(opId) || std::any_of(items.begin(), items.end(), isOKItemForPair)) {
            // If 'op' has a successful result recorded already or if 'items' has an OK item, then
            // remove all CollectionUUIDMismatch errors without an actual namespace from 'items'.
            std::erase_if(items, [&](auto&& p) {
                return isCollUUIDMismatchWithoutActualNamespace(p.second);
            });

            // If 'items' is now empty, remove 'op' from 'itemMap'.
            if (items.empty()) {
                itemMap.erase(op);
            }
        }
    }

    return itemMap;
}

void WriteBatchResponseProcessor::processCountersAndRetriedStmtIds(
    const BatchWriteCommandReply& parsedReply) {
    // Update the counters.
    _nInserted += parsedReply.nInserted;
    _nMatched += parsedReply.nMatched;
    _nModified += parsedReply.nModified;
    _nUpserted += parsedReply.nUpserted;
    _nDeleted += parsedReply.nDeleted;

    // Update the list of retried stmtIds.
    for (auto retriedStmtId : parsedReply.retriedStmtIds) {
        _retriedStmtIds.insert(retriedStmtId);
    }
}

void WriteBatchResponseProcessor::processCountersAndRetriedStmtIds(
    const BulkWriteCommandReply& parsedReply) {
    // Update the counters.
    _nInserted += parsedReply.getNInserted();
    _nMatched += parsedReply.getNMatched();
    _nModified += parsedReply.getNModified();
    _nUpserted += parsedReply.getNUpserted();
    _nDeleted += parsedReply.getNDeleted();

    // Update the list of retried stmtIds.
    if (auto retriedStmtIds = parsedReply.getRetriedStmtIds();
        retriedStmtIds && !retriedStmtIds->empty()) {
        for (auto retriedStmtId : *retriedStmtIds) {
            _retriedStmtIds.insert(retriedStmtId);
        }
    }
}

void WriteBatchResponseProcessor::retrieveBatchWriteReplyItems(OperationContext* opCtx,
                                                               RoutingContext& routingCtx,
                                                               const ShardId& shardId,
                                                               const std::vector<WriteOp>& ops,
                                                               ShardResult& result) {
    tassert(11468110, "Expected BatchWriteCommandReply", result.batchWriteReply.has_value());
    const BatchWriteCommandReply& parsedReply = *result.batchWriteReply;

    // Validate the reply items.
    validateBatchWriteReplyItems(opCtx, parsedReply.items, ops.size());

    retrieveReplyItemsImpl(opCtx, routingCtx, ops, parsedReply.items, result);
}

void WriteBatchResponseProcessor::retrieveBulkWriteReplyItems(OperationContext* opCtx,
                                                              RoutingContext& routingCtx,
                                                              const ShardId& shardId,
                                                              const std::vector<WriteOp>& ops,
                                                              bool errorsOnly,
                                                              ShardResult& result) {
    tassert(11182209, "Expected BulkWriteCommandReply", result.bulkWriteReply.has_value());
    const BulkWriteCommandReply& parsedReply = *result.bulkWriteReply;

    // Put all the reply items into a vector and then validate the reply items. Note that if the
    // BulkWriteCommandReply object contains a non-zero cursor ID, exhaustCursorForReplyItems()
    // will send commands over the network as needed to retrieve all the reply items from the
    // shard.
    auto items = exhaustCursorForReplyItems(opCtx, shardId, parsedReply);
    validateBulkWriteReplyItems(opCtx, items, ops.size(), errorsOnly);

    retrieveReplyItemsImpl(opCtx, routingCtx, ops, items, result);
}

void WriteBatchResponseProcessor::retrieveReplyItemsImpl(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    const std::vector<WriteOp>& ops,
    const std::vector<BulkWriteReplyItem>& items,
    ShardResult& result) {
    const bool orderedOrInTxn = _cmdRef.getOrdered() || TransactionRouter::get(opCtx);
    const bool isBulkWrite = _cmdRef.isBulkWriteCommand();

    bool assumeOpsWithNoItemWereExecuted = true;
    boost::optional<Status> finalRetryableErrorForLogging;
    std::vector<WriteOp> opsAfterFinalRetryableError;

    size_t itemIndex = 0;

    for (size_t shardOpId = 0; shardOpId < ops.size(); ++shardOpId) {
        const auto& op = ops[shardOpId];

        // Check if we have a reply item for 'shardOpId'.
        if (itemIndex < items.size() &&
            shardOpId == static_cast<size_t>(items[itemIndex].getIdx())) {
            // Handle the case where we have a reply item.
            auto item = items[itemIndex];
            ++itemIndex;

            // Update item's idx value to refer to the ID for this op from the original
            // write command issued by the client.
            item.setIdx(getWriteOpId(op));

            if (const auto& status = item.getStatus(); !status.isOK()) {
                const bool isRetryableErr = write_op_helpers::isRetryErrCode(status.code());

                // If a retryable error occurred, inform 'routingCtx' if needed.
                if (isRetryableErr) {
                    noteRetryableError(opCtx, routingCtx, status);
                }

                const bool isFinalRetryableErrorInBulkWriteReply =
                    isBulkWrite && isRetryableErr && itemIndex >= items.size();

                // If 'orderedOrInTxn' or 'isFinalRetryableErrorInBulkWriteReply' is true, then
                // we assume the remaining ops without reply items did not execute.
                if (orderedOrInTxn || isFinalRetryableErrorInBulkWriteReply) {
                    assumeOpsWithNoItemWereExecuted = false;
                }

                // If 'isFinalRetryableErrorInBulkWriteReply' is true and 'shouldLog()' returns
                // true, store the error into 'finalRetryableErrorForLogging'.
                if (isFinalRetryableErrorInBulkWriteReply &&
                    shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, logv2::LogSeverity::Debug(4))) {
                    finalRetryableErrorForLogging.emplace(status);
                }
            }

            result.items.emplace_back(op, std::move(item));
        } else {
            // Handle the case where we don't have a reply item for 'shardOpId'.
            if (assumeOpsWithNoItemWereExecuted) {
                result.items.emplace_back(op, SucceededWithoutItem{});
            } else {
                result.items.emplace_back(op, Unexecuted{});

                // Add 'op' to 'opsAfterFinalRetryableError' if 'finalRetryableErrorForLogging'
                // is set.
                if (finalRetryableErrorForLogging) {
                    opsAfterFinalRetryableError.emplace_back(op);
                }
            }
        }
    }

    // Log the contents of 'opsAfterFinalRetryableError' if it's not empty.
    if (finalRetryableErrorForLogging && !opsAfterFinalRetryableError.empty()) {
        std::stringstream opsStream;
        size_t numOpsInStream = 0;
        for (const auto& op : opsAfterFinalRetryableError) {
            opsStream << (numOpsInStream++ > 0 ? ", " : "") << getWriteOpId(op);
        }

        LOGV2_DEBUG(11182210,
                    4,
                    "Retryable error occurred during batch, op(s) may need to be retried",
                    "opIdx"_attr = opsStream.str(),
                    "error"_attr = *finalRetryableErrorForLogging);
    }
}

void WriteBatchResponseProcessor::validateBatchWriteReplyItems(
    OperationContext* opCtx, const std::vector<BulkWriteReplyItem>& items, size_t numOps) {
    for (size_t i = 0; i < items.size(); ++i) {
        auto& item = items[i];
        tassert(11468115,
                fmt::format("Shard replied with invalid opId {}", item.getIdx()),
                item.getIdx() >= 0 && static_cast<size_t>(item.getIdx()) < numOps);
    }
}

void WriteBatchResponseProcessor::validateBulkWriteReplyItems(
    OperationContext* opCtx,
    const std::vector<BulkWriteReplyItem>& items,
    size_t numOps,
    bool errorsOnly) {
    const bool ordered = _cmdRef.getOrdered();
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));

    boost::optional<size_t> lastIdx;
    bool hasErrorItem = false;
    for (size_t i = 0; i < items.size(); ++i) {
        auto& item = items[i];
        tassert(10347004,
                fmt::format("Shard replied with invalid opId {}", item.getIdx()),
                item.getIdx() >= 0 && static_cast<size_t>(item.getIdx()) < numOps);

        size_t idx = item.getIdx();
        tassert(11182211, "Expected opIds to be in increasing order", !lastIdx || *lastIdx < idx);
        tassert(11182212,
                fmt::format("Expected reply item to exist for opId {}", i),
                errorsOnly || i == idx);

        if (!item.getStatus().isOK()) {
            hasErrorItem = true;

            tassert(11182213,
                    "bulkWrite should not see reply items after an error when 'ordered' is true",
                    !ordered || i + 1 == items.size());
        }

        lastIdx = idx;
    }

    if (!errorsOnly && items.size() < numOps) {
        tassert(11182214,
                "bulkWrite should always get replies when 'errorsOnly' is false",
                !items.empty());

        const bool lastItemIsRetryableError =
            write_op_helpers::isRetryErrCode(items.back().getStatus().code());

        tassert(11182215,
                fmt::format("Expected reply item to exist for opId {}", items.size()),
                hasErrorItem && (ordered || inTransaction || lastItemIsRetryableError));
    }
}

namespace {
BulkWriteReplyItem getFirstError(const std::vector<BulkWriteReplyItem>& items) {
    tassert(11182216, "Expected vector to contain at least one item", !items.empty());

    // If 'items' has multiple errors and the first error is a CollectionUUIDMismatch error, then
    // check if 'items' contains a CollectionUUIDMismatch error with actualCollection() set, and if
    // so return that.
    if (items.size() > 1 && items.front().getStatus() == ErrorCodes::CollectionUUIDMismatch) {
        for (const auto& item : items) {
            auto info = getCollectionUUIDMismatchInfo(item.getStatus());
            if (info && info->actualCollection()) {
                return item;
            }
        }
    }

    // Return the first error in 'items'.
    return items.front();
}

BulkWriteReplyItem combineSuccessfulReplies(WriteOpId opId, std::vector<BulkWriteReplyItem> items) {
    tassert(11182217, "Expected vector to contain at least one item", !items.empty());

    if (items.size() == 1) {
        return items.front();
    }

    BulkWriteReplyItem combinedReply;
    combinedReply.setOk(1);
    combinedReply.setIdx(opId);

    for (const auto& reply : items) {
        if (auto n = reply.getN(); n.has_value()) {
            combinedReply.setN(combinedReply.getN().get_value_or(0) + n.value());
        }
        if (auto nModified = reply.getNModified(); nModified.has_value()) {
            combinedReply.setNModified(combinedReply.getNModified().get_value_or(0) +
                                       nModified.value());
        }
        if (auto upserted = reply.getUpserted(); upserted.has_value()) {
            tassert(10412300,
                    "Unexpectedly got bulkWrite upserted reply items from multiple shards for a "
                    "single update operation",
                    !combinedReply.getUpserted().has_value());
            combinedReply.setUpserted(reply.getUpserted());
        }
    }

    return combinedReply;
}

BulkWriteReplyItem combineErrorReplies(WriteOpId opId, std::vector<BulkWriteReplyItem> items) {
    tassert(11182218, "Expected vector to contain at least one item", !items.empty());

    auto getStatusCode = [](auto&& item) {
        return item.getStatus().code();
    };

    // Handle the case where there is only one error reply, or where all error replies have the
    // same error code.
    if (items.size() == 1 || write_op_helpers::errorsAllSame(items, getStatusCode)) {
        return getFirstError(items);
    }

    // Handle the case where there is exactly one non-retryable error reply.
    if (write_op_helpers::hasOnlyOneNonRetryableError(items, getStatusCode)) {
        return write_op_helpers::getFirstNonRetryableError(items, getStatusCode);
    }

    bool skipRetryableErrors = !write_op_helpers::hasAnyNonRetryableError(items, getStatusCode);

    // Generate the multi-error message below.
    std::stringstream msg("multiple errors for op : ");
    bool firstError = true;
    BSONArrayBuilder errB;
    for (std::vector<BulkWriteReplyItem>::const_iterator it = items.begin(); it != items.end();
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

/**
 * If 'error' is a CollectionUUIDMismatch error without an actual namespace, this function attempts
 * to populate the "actualCollection" field on 'error', contacting the primary shard if necessary.
 * Otherwise, this function does nothing.
 */
void addActualNamespaceForCollUUIDMismatchError(OperationContext* opCtx,
                                                Status& error,
                                                boost::optional<std::string>& actualCollection,
                                                bool& hasContactedPrimaryShard) {
    // Return early if 'error' is not a CollectionUUIDMismatch error or if it's not missing the
    // "actualCollection" field.
    auto info = getCollectionUUIDMismatchInfo(error);
    if (!info || info->actualCollection()) {
        return;
    }

    LOGV2_DEBUG(11273501,
                4,
                "Encountered collection uuid mismatch when processing errors",
                "error"_attr = redact(error));

    if (actualCollection) {
        // If the 'actualCollection' parameter is set, add it to 'error' and return.
        error = Status{CollectionUUIDMismatchInfo{info->dbName(),
                                                  info->collectionUUID(),
                                                  info->expectedCollection(),
                                                  *actualCollection},
                       error.reason()};
    } else if (!hasContactedPrimaryShard) {
        // Otherwise, if 'hasContactedPrimaryShard', try contacting the primary shard to get the
        // "actualCollection", and if successful add it to 'error' and return.
        error = populateCollectionUUIDMismatch(opCtx, error);

        if (error == ErrorCodes::CollectionUUIDMismatch) {
            hasContactedPrimaryShard = true;

            if (auto& populatedActualCollection =
                    getCollectionUUIDMismatchInfo(error)->actualCollection()) {
                actualCollection = populatedActualCollection;
            }
        }
    }
}
}  // namespace

void WriteBatchResponseProcessor::updateApproximateSize(const BulkWriteReplyItem& item) {
    // If the response is not an error and 'errorsOnly' is set, then we should not store this
    // reply and therefore shouldn't count the size.
    if (item.getOk() && _cmdRef.getErrorsOnly().value_or(false)) {
        return;
    }

    _approximateSize += item.getApproximateSize();
}

void WriteBatchResponseProcessor::recordTargetErrors(OperationContext* opCtx,
                                                     const BatcherResult& batcherResult) {
    handleTransientTxnError(opCtx, batcherResult);

    // If the write command is ordered or running in a transaction, the batcher should return
    // at most one target error.
    const bool orderedOrInTxn = _cmdRef.getOrdered() || TransactionRouter::get(opCtx);
    tassert(11182219,
            "Unexpectedly received multiple errors from the batcher",
            batcherResult.opsWithErrors.size() <= 1 || !orderedOrInTxn);

    // Record the target errors.
    for (const auto& [op, status] : batcherResult.opsWithErrors) {
        recordError(opCtx, op, status);
    }
}

bool WriteBatchResponseProcessor::opHasSuccess(WriteOpId opId) const {
    auto it = _results.find(opId);
    if (it == _results.end()) {
        return false;
    }

    auto visitItem = OverloadedVisitor(
        [&](const BulkWriteOpResults& result) { return result.hasSuccess; },
        [&](const FindAndModifyReplyItem& result) { return result.swReply.getStatus().isOK(); });

    const auto& resultVar = it->second;
    return visit(std::move(visitItem), resultVar);
}

bool WriteBatchResponseProcessor::checkBulkWriteReplyMaxSize(OperationContext* opCtx) {
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
        // Create an ExceededMemoryLimit error.
        WriteOp nextHighestWriteOp(_cmdRef.getOp(nextHighestWriteOpId));
        Status exceededMemLimitError{
            ErrorCodes::ExceededMemoryLimit,
            fmt::format("BulkWrite response size exceeded limit ({} bytes)", _approximateSize)};

        // Record this error with the next highest write op ID.
        recordError(opCtx, nextHighestWriteOp, std::move(exceededMemLimitError));
        return true;
    }

    return false;
}

std::vector<std::pair<WriteOpId, boost::optional<BulkWriteReplyItem>>>
WriteBatchResponseProcessor::finalizeRepliesForOps(OperationContext* opCtx) {
    std::vector<std::pair<WriteOpId, boost::optional<BulkWriteReplyItem>>> aggregatedReplies;
    std::map<NamespaceString, boost::optional<std::string>> actualCollections;
    std::map<NamespaceString, bool> hasContactedPrimaryShard;
    bool hasCollUUIDMismatchErrorsWithoutActualNamespace = false;

    for (const auto& nss : _cmdRef.getNssSet()) {
        actualCollections.emplace(nss, boost::none);
        hasContactedPrimaryShard.emplace(nss, false);
    }

    // For CollectionUUIDMismatch errors, this lambda checks if "actualCollection" is set, and
    // it updates the 'actualCollection' and 'hasCollUUIDMismatchErrorsWithoutActualNamespace'
    // variables appropriately.
    auto noteCollUUIDMismatchError = [&](const WriteOp& op, const Status& status) {
        auto info = getCollectionUUIDMismatchInfo(status);
        if (!info) {
            return;
        }

        if (info->actualCollection()) {
            if (auto& actualCollection = actualCollections[op.getNss()]; !actualCollection) {
                actualCollection = info->actualCollection();
            }
        } else {
            hasCollUUIDMismatchErrorsWithoutActualNamespace = true;
        }
    };

    for (const auto& [opId, opResultVar] : _results) {
        auto* opResults = get_if<BulkWriteOpResults>(&opResultVar);
        tassert(11182220, "Expected BulkWriteOpResults", opResults != nullptr);

        const auto& items = opResults->items;
        auto op = WriteOp{_cmdRef.getOp(opId)};

        if (opResults->hasSuccess && opResults->hasError) {
            // Handle the case where we have a combination of errors and successful replies.
            std::vector<BulkWriteReplyItem> successfulReplies;
            std::vector<BulkWriteReplyItem> errorReplies;

            for (const auto& item : items) {
                if (item.getStatus().isOK()) {
                    successfulReplies.push_back(item);
                } else {
                    errorReplies.push_back(item);
                }
            }

            auto reply = boost::make_optional(combineErrorReplies(opId, std::move(errorReplies)));

            // We must never ignore a WCOS error. Instead, we will surface it here.
            if (reply->getStatus() == ErrorCodes::WouldChangeOwningShard) {
                aggregatedReplies.emplace_back(opId, std::move(reply));
                continue;
            }

            if (reply && reply->getStatus() == ErrorCodes::CollectionUUIDMismatch) {
                noteCollUUIDMismatchError(op, reply->getStatus());
            }

            if (!successfulReplies.empty()) {
                auto successReply = combineSuccessfulReplies(opId, std::move(successfulReplies));

                if (reply) {
                    reply->setN(successReply.getN());
                    reply->setNModified(successReply.getNModified());
                    reply->setUpserted(successReply.getUpserted());
                } else {
                    reply = std::move(successReply);
                }
            }

            aggregatedReplies.emplace_back(opId, std::move(reply));
        } else if (opResults->hasError) {
            // Handle the case where we have errors and no successful replies.
            auto reply = combineErrorReplies(opId, items);

            if (reply.getStatus() == ErrorCodes::CollectionUUIDMismatch) {
                noteCollUUIDMismatchError(op, reply.getStatus());
            }

            aggregatedReplies.emplace_back(opId, std::move(reply));
        } else if (opResults->hasSuccess) {
            // Handle the case where we have successful replies and no errors.
            if (!items.empty()) {
                aggregatedReplies.emplace_back(opId, combineSuccessfulReplies(opId, items));
            } else {
                aggregatedReplies.emplace_back(opId, boost::none);
            }
        }
    }

    // If there were any CollectionUUIDMismatch errors without an actual namespace, then call
    // addActualNamespaceForCollUUIDMismatchError() to populate the "actualCollection" field.
    if (hasCollUUIDMismatchErrorsWithoutActualNamespace) {
        for (auto& [opId, item] : aggregatedReplies) {
            if (item && item->getStatus() == ErrorCodes::CollectionUUIDMismatch) {
                auto op = WriteOp{_cmdRef.getOp(opId)};
                const NamespaceString& nss = op.getNss();

                auto error = item->getStatus();
                addActualNamespaceForCollUUIDMismatchError(
                    opCtx, error, actualCollections[nss], hasContactedPrimaryShard[nss]);

                item->setStatus(std::move(error));
            }
        }
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

bulk_write_exec::BulkWriteReplyInfo
WriteBatchResponseProcessor::generateClientResponseForBulkWriteCommand(OperationContext* opCtx) {
    const bool errorsOnly = _cmdRef.getErrorsOnly().value_or(false);

    std::vector<BulkWriteReplyItem> results;

    auto finalResults = finalizeRepliesForOps(opCtx);

    for (auto& [id, item] : finalResults) {
        // Check to see if we've recieved a WCOS error. If we have, then we should skip incrementing
        // the relevant op counters as this will double count our update. Instead, we will increment
        // this as part of the WCOS handling in the command layer.
        const bool isNotAWCOSError = !item || item->getStatus().isOK() ||
            item->getStatus() != ErrorCodes::WouldChangeOwningShard || _cmdRef.getNumOps() > 1;
        if (!_isNonVerbose && (!errorsOnly || (item && !item->getStatus().isOK()))) {
            tassert(11182221, "Expected a BulkWriteReplyItem", item.has_value());

            // The item's 'idx' should correspond to the op's ID from the original client request.
            tassert(10347002,
                    fmt::format("expected id in reply ({}) to match id of operation from "
                                "original request ({})",
                                item->getIdx(),
                                id),
                    static_cast<WriteOpId>(item->getIdx()) == id);
            results.emplace_back(std::move(*item));
        }
        _stats.incrementOpCounters(opCtx, _cmdRef.getOp(id), isNotAWCOSError);
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
    return info;
}

BulkWriteCommandReply WriteBatchResponseProcessor::generateClientResponseForBulkWriteForTest(
    OperationContext* opCtx) {
    return populateCursorReply(opCtx,
                               _cmdRef.getBulkWriteCommandRequest(),
                               _originalCommand,
                               generateClientResponseForBulkWriteCommand(opCtx));
}

BatchedCommandResponse WriteBatchResponseProcessor::generateClientResponseForBatchedCommand(
    OperationContext* opCtx) {
    BatchedCommandResponse resp;
    resp.setStatus(Status::OK());

    // For non-verbose batched command requests, we always return an OK response with n=0. This
    // matches the behavior of BatchWriteOp::buildClientResponse().
    if (_isNonVerbose) {
        return resp;
    }

    // We call finalizeRepliesForOps() to aggregate replies from different shards for a single op.
    auto finalResults = finalizeRepliesForOps(opCtx);

    for (const auto& [id, item] : finalResults) {
        if (item && !item->getStatus().isOK()) {
            resp.addToErrDetails(write_ops::WriteError(id, item->getStatus()));
        }

        if (item) {
            // Verify that the id matches the one from the original client request.
            tassert(10605504,
                    fmt::format("expected id in reply ({}) to match id of operation from "
                                "original request ({})",
                                item->getIdx(),
                                id),
                    static_cast<WriteOpId>(item->getIdx()) == id);

            // Handle propagating 'upsertedId' information.
            if (const auto& upserted = item->getUpserted(); upserted) {
                auto detail = std::make_unique<BatchedUpsertDetail>();
                detail->setIndex(id);

                BSONObjBuilder upsertedObjBuilder;
                upserted->serializeToBSON("_id", &upsertedObjBuilder);
                detail->setUpsertedID(upsertedObjBuilder.done());

                resp.addToUpsertDetails(detail.release());
            }
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
    tassert(10394906, "Expected exactly one findAndModify reply item", _results.size() == 1);

    const auto* reply = get_if<FindAndModifyReplyItem>(&_results.begin()->second);
    tassert(11182222, "Expected FindAndModifyReplyItem", reply != nullptr);

    boost::optional<WriteConcernErrorDetail> wce = boost::none;
    if (auto totalWcError = mergeWriteConcernErrors(_wcErrors); totalWcError) {
        wce = WriteConcernErrorDetail{totalWcError->toStatus()};
    }

    return FindAndModifyCommandResponse{reply->swReply, reply->shardId, std::move(wce)};
}

}  // namespace mongo::unified_write_executor
