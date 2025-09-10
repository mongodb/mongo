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
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/unified_write_executor/write_batch_executor.h"
#include "mongo/util/assert_util.h"

#include <variant>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::unified_write_executor {
using Result = WriteBatchResponseProcessor::Result;

Result WriteBatchResponseProcessor::onWriteBatchResponse(OperationContext* opCtx,
                                                         RoutingContext& routingCtx,
                                                         const WriteBatchResponse& response) {
    return std::visit(
        [&](const auto& responseData) -> Result {
            return _onWriteBatchResponse(opCtx, routingCtx, responseData);
        },
        response);
}

Result WriteBatchResponseProcessor::_onWriteBatchResponse(
    OperationContext* opCtx, RoutingContext& routingCtx, const SimpleWriteBatchResponse& response) {
    Result result;
    for (const auto& [shardId, shardResponse] : response) {
        auto shardResult = onShardResponse(opCtx, routingCtx, shardId, shardResponse);
        result.opsToRetry.insert(result.opsToRetry.end(),
                                 std::make_move_iterator(shardResult.opsToRetry.begin()),
                                 std::make_move_iterator(shardResult.opsToRetry.end()));
        for (auto& [nss, info] : shardResult.collsToCreate) {
            if (auto it = result.collsToCreate.find(nss); it == result.collsToCreate.cend()) {
                result.collsToCreate.emplace(nss, std::move(info));
            }
        }

        if (shardResult.errorType != ErrorType::kNone) {
            result.errorType = shardResult.errorType;

            // If 'errorType' is kStopProcessing, stop processing immediately and return a Result
            // for what we've processed so far.
            if (shardResult.errorType == ErrorType::kStopProcessing) {
                break;
            }
        }
    }
    return result;
}

Result WriteBatchResponseProcessor::_onWriteBatchResponse(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    const NonTargetedWriteBatchResponse& response) {
    // TODO SERVER-104535 cursor support for UnifiedWriteExec.
    // TODO SERVER-104122 Support for 'WouldChangeOwningShard' writes.
    // TODO SERVER-105762 Add support for errorsOnly: true.
    const auto& swRes = response.swResponse;
    const auto& op = response.op;
    if (!swRes.getStatus().isOK()) {
        return handleLocalError(opCtx, swRes.getStatus(), op, boost::none /* shardId */);
    }

    // Extract the reply item from the ClusterWriteWithoutShardKeyResponse if possible, otherwise
    // create a reply item.
    BulkWriteReplyItem replyItem = [&] {
        if (swRes.isOK() && !swRes.getValue().getResponse().isEmpty()) {
            auto parsedReply = BulkWriteCommandReply::parse(
                swRes.getValue().getResponse(),
                IDLParserContext("BulkWriteCommandReply_UnifiedWriteExec"));

            // Update the counters.
            _nInserted += parsedReply.getNInserted();
            _nDeleted += parsedReply.getNDeleted();
            _nMatched += parsedReply.getNMatched();
            _nUpserted += parsedReply.getNUpserted();
            _nModified += parsedReply.getNModified();

            const auto& replyItems = parsedReply.getCursor().getFirstBatch();
            tassert(10378000, "Unexpected reply for NonTargetedWriteBatch", replyItems.size() == 1);

            const auto& replyItem = parsedReply.getCursor().getFirstBatch().front();
            tassert(10378001,
                    fmt::format("reply with invalid opId {} when command only had 1 op",
                                replyItem.getIdx()),
                    static_cast<WriteOpId>(replyItem.getIdx()) == 0);

            return replyItem;
        }

        // If we reach here, then either:
        //   1) 'swRes' is not OK (which means an error occurred); or
        //   2) 'swRes' is OK but 'response' is empty (which means the two-phase write completed
        //      successfully without updating/deleting anything because nothing matched the filter).
        //
        // In either case, we create a reply item with the status from 'swRes' and we set n=0
        // and nModified=0 (if 'op' is an update) or just n=0 (if 'op' is a delete).
        BulkWriteReplyItem replyItem(0, swRes.getStatus());
        replyItem.setN(0);
        if (op.getType() == WriteType::kUpdate) {
            replyItem.setNModified(0);
        }

        return replyItem;
    }();

    if (!replyItem.getStatus().isOK()) {
        _nErrors++;
    }

    auto [it, _] = _results.emplace(op.getId(), std::move(replyItem));
    it->second.setIdx(op.getId());

    // NonTargetedWriteBatches are only used for update and delete, so we never need to implicitly
    // create collections for a TwoPhaseWrite op.
    //
    // Also, the write_without_shard_key::runTwoPhaseWriteProtocol() API handles StaleConfig
    // responses internally, so the UnifiedWriteExecutor doesn't need to have retry logic for
    // TwoPhaseWrite operations.
    return {};
}


void WriteBatchResponseProcessor::noteErrorResponseOnAbort(int opId, const Status& status) {
    tassert(10413102, "Unexpectedly got an OK status", !status.isOK());
    _results.emplace(opId, BulkWriteReplyItem(opId, status));
    _nErrors++;
}

Result WriteBatchResponseProcessor::handleLocalError(OperationContext* opCtx,
                                                     Status status,
                                                     WriteOp op,
                                                     boost::optional<const ShardId&> shardId) {
    // TODO SERVER-105303 Handle interruption/shutdown.
    // TODO SERVER-104122 Support for 'WouldChangeOwningShard' writes.
    // If we are in a transaction, we stop processing and return the first error.
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    if (inTransaction) {
        auto newStatus = status.withContext(
            str::stream() << "Encountered error from " << (shardId ? shardId->toString() : "shards")
                          << " during a transaction");
        // Transient transaction errors should be returned directly as top level errors to allow
        // the client to retry.
        if (isTransientTransactionError(
                status.code(), /*hasWriteConcernError*/ false, /*isCommitOrAbort*/ false)) {
            uassertStatusOK(newStatus);
        }
        // TODO SERVER-105762 Figure out what opId to use here in case we don't return any.
        LOGV2_DEBUG(10413100,
                    4,
                    "Aborting batch due to error in transaction",
                    "error"_attr = redact(newStatus));
        noteErrorResponseOnAbort(op.getId(), status);
        return {ErrorType::kStopProcessing, {}, {}};
    }
    noteErrorResponseOnAbort(op.getId(), status);
    return {ErrorType::kUnrecoverable};
}

Result WriteBatchResponseProcessor::onShardResponse(OperationContext* opCtx,
                                                    RoutingContext& routingCtx,
                                                    const ShardId& shardId,
                                                    const ShardResponse& response) {
    const Status& status = response.swResponse.getStatus();
    const auto& ops = response.ops;
    if (!status.isOK()) {
        return handleLocalError(opCtx, status, ops.front(), shardId);
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

        // Processing stale error returned as a top-level error.
        if (status == ErrorCodes::StaleDbVersion || ErrorCodes::isStaleShardVersionError(status)) {
            routingCtx.onStaleError(status);
        }

        // If we are in a transaction, we stop processing and return the first error.
        const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
        if (inTransaction) {
            auto errorReply = ErrorReply::parse(shardResponse.data, IDLParserContext("ErrorReply"));
            // Transient transaction errors should be returned directly as top level errors to allow
            // the client to retry.
            if (hasTransientTransactionErrorLabel(errorReply)) {
                uassertStatusOK(status.withContext(str::stream() << "Encountered error from "
                                                                 << shardResponse.target
                                                                 << " during a transaction"));
            }
            // TODO SERVER-105762 Figure out what opId to use here in case we don't return any.
            LOGV2_DEBUG(10413101,
                        4,
                        "Aborting batch due to error in a transaction",
                        "error"_attr = redact(status),
                        "host"_attr = shardResponse.target);
            noteErrorResponseOnAbort(ops.front().getId(), status);
            return {ErrorType::kStopProcessing, {}, {}};
        }

        for (const auto& op : ops) {
            auto [it, _] = _results.emplace(op.getId(), BulkWriteReplyItem(op.getId(), status));
            it->second.setIdx(op.getId());
        }
        LOGV2_DEBUG(10347001,
                    4,
                    "Unable to receive cluster write results from shard",
                    "host"_attr = shardResponse.target);

        _nErrors += ops.size();
        return {};
    }

    // Parse and handle inner ok and error responses.
    auto parsedReply = BulkWriteCommandReply::parse(
        shardResponse.data, IDLParserContext("BulkWriteCommandReply_UnifiedWriteExec"));

    // Process write concern error
    auto wcError = parsedReply.getWriteConcernError();
    if (wcError) {
        _wcErrors.push_back(ShardWCError(
            shardId, {Status(ErrorCodes::Error(wcError->getCode()), wcError->getErrmsg())}));
    }

    _nInserted += parsedReply.getNInserted();
    _nDeleted += parsedReply.getNDeleted();
    _nMatched += parsedReply.getNMatched();
    _nUpserted += parsedReply.getNUpserted();
    _nModified += parsedReply.getNModified();

    if (auto retriedStmtIds = parsedReply.getRetriedStmtIds();
        retriedStmtIds && !retriedStmtIds->empty()) {
        for (auto retriedStmtId : *retriedStmtIds) {
            _retriedStmtIds.insert(retriedStmtId);
        }
    }

    // TODO SERVER-104535 cursor support for UnifiedWriteExec.
    const auto& replyItems = parsedReply.getCursor().getFirstBatch();
    auto result = processOpsInReplyItems(opCtx, routingCtx, ops, replyItems);
    if (result.errorType == ErrorType::kNone) {
        result.opsToRetry =
            processOpsNotInReplyItems(ops, replyItems, std::move(result.opsToRetry));
    }
    return result;
}

Result WriteBatchResponseProcessor::processOpsInReplyItems(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    const std::vector<WriteOp>& ops,
    const std::vector<BulkWriteReplyItem>& replyItems) {
    std::vector<WriteOp> toRetry;
    CollectionsToCreate collectionsToCreate;
    ErrorType errorType = ErrorType::kNone;
    for (const auto& item : replyItems) {
        // TODO SERVER-104122 Support for 'WouldChangeOwningShard' writes.
        tassert(10347004,
                fmt::format("shard replied with invalid opId {} when it was only sent {} ops",
                            item.getIdx(),
                            ops.size()),
                static_cast<WriteOpId>(item.getIdx()) < ops.size());
        const auto& op = ops[item.getIdx()];

        const auto itemCode = item.getStatus().code();
        if (itemCode == ErrorCodes::CannotImplicitlyCreateCollection) {
            // Stage the collection to be created if it was found to not exist.
            auto info = item.getStatus().extraInfo<CannotImplicitlyCreateCollectionInfo>();
            if (auto it = collectionsToCreate.find(info->getNss());
                it == collectionsToCreate.cend()) {
                collectionsToCreate.emplace(info->getNss(), std::move(info));
            }
            toRetry.push_back(op);
        } else if (itemCode == ErrorCodes::StaleDbVersion ||
                   ErrorCodes::isStaleShardVersionError(itemCode)) {
            if (itemCode == ErrorCodes::StaleDbVersion) {
                LOGV2_DEBUG(10411403,
                            4,
                            "Noting stale database response",
                            "status"_attr = item.getStatus());
            } else {
                LOGV2_DEBUG(
                    10346900, 4, "Noting stale config response", "status"_attr = item.getStatus());
            }
            routingCtx.onStaleError(item.getStatus(), op.getNss());
            toRetry.push_back(op);
        } else if (itemCode == ErrorCodes::ShardCannotRefreshDueToLocksHeld) {
            LOGV2_DEBUG(10413104,
                        4,
                        "Noting shard cannot refresh due to locks held response",
                        "status"_attr = item.getStatus());
            toRetry.push_back(op);
        } else {
            auto [it, _] = _results.emplace(op.getId(), item);
            it->second.setIdx(op.getId());

            auto status = item.getStatus();
            if (!status.isOK()) {
                // Attempts to populate the actualCollection field of a CollectionUUIDMismatch error
                // if not already present.
                if (status.code() == ErrorCodes::CollectionUUIDMismatch) {
                    status = populateCollectionUUIDMismatch(opCtx, status);
                }

                _nErrors++;
                // If we are in a transaction, we stop processing and return the first error.
                if (static_cast<bool>(TransactionRouter::get(opCtx))) {
                    LOGV2_DEBUG(10413103,
                                4,
                                "Aborting batch due to error in a transaction",
                                "error"_attr = redact(status));
                    errorType = ErrorType::kStopProcessing;
                    break;
                }
                if (op.getCommand().getOrdered()) {
                    errorType = ErrorType::kUnrecoverable;
                }
            }
        }
    }
    return {errorType, std::move(toRetry), collectionsToCreate};
}

std::vector<WriteOp> WriteBatchResponseProcessor::processOpsNotInReplyItems(
    const std::vector<WriteOp>& requestedOps,
    const std::vector<BulkWriteReplyItem>& replyItems,
    std::vector<WriteOp>&& toRetry) {
    if (requestedOps.size() != replyItems.size()) {
        // TODO SERVER-105762 Add support for errorsOnly: true.
        // If we are here it means we got a response from an ordered: true command and it stopped on
        // the first error.
        for (size_t i = replyItems.size(); i < requestedOps.size(); i++) {
            LOGV2_DEBUG(10411404,
                        4,
                        "renenqueuing op not completed by shard",
                        "op"_attr = requestedOps[i].getId());
            toRetry.push_back(requestedOps[i]);
        }
    }
    return toRetry;
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
    // behavior of ClusterBulkWriteCmd::Invocation::_populateCursorReply().
    std::vector<BulkWriteReplyItem> results;

    for (const auto& [id, item] : _results) {
        if (!_isNonVerbose) {
            results.push_back(item);
            // Set the Idx to be the one from the original client request.
            tassert(
                10347002,
                fmt::format(
                    "expected id in reply ({}) to match id of operation from original request ({})",
                    item.getIdx(),
                    id),
                static_cast<WriteOpId>(item.getIdx()) == id);
        }
        // TODO SERVER-104123 Handle multi: true case where we have multiple reply items for the
        // same op id from the original client request.
        _stats.incrementOpCounters(opCtx, _cmdRef.getOp(id));
    }

    // Construct a BulkWriteCommandReply object. We always store the values of the top-level
    // counters for the command (nInserted, nMatched, etc) into the BulkWriteCommandReply object,
    // regardless of whether '_isNonVerbose' is true or false.
    auto reply = BulkWriteCommandReply(
        // TODO SERVER-104535 cursor support for UnifiedWriteExec.
        BulkWriteCommandResponseCursor(
            0, std::move(results), NamespaceString::makeBulkWriteNSS(boost::none)),
        _nErrors,
        _nInserted,
        _nMatched,
        _nModified,
        _nUpserted,
        _nDeleted);
    reply.setRetriedStmtIds(getRetriedStmtIds());

    // Aggregate all the write concern errors from the shards.
    if (auto totalWcError = mergeWriteConcernErrors(_wcErrors); totalWcError) {
        reply.setWriteConcernError(BulkWriteWriteConcernError{totalWcError->toStatus().code(),
                                                              totalWcError->toStatus().reason()});
    }

    return reply;
}

BatchedCommandResponse WriteBatchResponseProcessor::generateClientResponseForBatchedCommand() {
    BatchedCommandResponse resp;
    resp.setStatus(Status::OK());

    // For non-verbose batched command requests, we always return an OK response with n=0.
    // This matches the behavior of BatchWriteOp::buildClientResponse().
    if (_isNonVerbose) {
        return resp;
    }

    for (const auto& [id, item] : _results) {
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
        // TODO SERVER-104123 Handle multi: true case where we have multiple reply items for the
        // same op id from the original client request.

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
    resp.setNModified(_nModified);
    resp.setRetriedStmtIds(getRetriedStmtIds());

    // Aggregate all the write concern errors from the shards.
    if (auto totalWcError = mergeWriteConcernErrors(_wcErrors); totalWcError) {
        resp.setWriteConcernError(new WriteConcernErrorDetail{totalWcError->toStatus()});
    }

    return resp;
}

}  // namespace mongo::unified_write_executor
