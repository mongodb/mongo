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

#include "mongo/s/write_ops/batched_command_response.h"

#include <variant>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::unified_write_executor {
using Result = WriteBatchResponseProcessor::Result;

Result WriteBatchResponseProcessor::onWriteBatchResponse(RoutingContext& routingCtx,
                                                         const WriteBatchResponse& response) {
    return std::visit(
        [&](const auto& responseData) -> Result {
            return _onWriteBatchResponse(routingCtx, responseData);
        },
        response);
}

Result WriteBatchResponseProcessor::_onWriteBatchResponse(
    RoutingContext& routingCtx, const SimpleWriteBatchResponse& response) {
    Result result;
    for (const auto& [shardId, shardResponse] : response) {
        auto shardResult = onShardResponse(routingCtx, shardId, shardResponse);
        result.unrecoverableError |= shardResult.unrecoverableError;
        result.opsToRetry.insert(result.opsToRetry.end(),
                                 std::make_move_iterator(shardResult.opsToRetry.begin()),
                                 std::make_move_iterator(shardResult.opsToRetry.end()));
        for (auto& [nss, info] : shardResult.collsToCreate) {
            if (auto it = result.collsToCreate.find(nss); it == result.collsToCreate.cend()) {
                result.collsToCreate.emplace(nss, std::move(info));
            }
        }
    }
    return result;
}

Result WriteBatchResponseProcessor::_onWriteBatchResponse(
    RoutingContext& routingCtx, const NonTargetedWriteBatchResponse& response) {
    // TODO SERVER-104115 retried stmts.
    // TODO SERVER-104535 cursor support for UnifiedWriteExec.
    // TODO SERVER-104122 Support for 'WouldChangeOwningShard' writes.
    // TODO SERVER-105762 Add support for errorsOnly: true.
    const auto& swRes = response.swResponse;
    const auto& op = response.op;

    // Extract the reply item from the ClusterWriteWithoutShardKeyResponse if possible, otherwise
    // create a reply item.
    BulkWriteReplyItem replyItem = [&] {
        if (swRes.isOK() && !swRes.getValue().getResponse().isEmpty()) {
            auto parsedReply = BulkWriteCommandReply::parse(
                IDLParserContext("BulkWriteCommandReply_UnifiedWriteExec"),
                swRes.getValue().getResponse());

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

Result WriteBatchResponseProcessor::onShardResponse(RoutingContext& routingCtx,
                                                    const ShardId& shardId,

                                                    const ShardResponse& response) {
    // Handle local errors, not from a shardResponse.
    if (!response.swResponse.isOK()) {
        // TODO SERVER-105303 Handle interruption/shutdown.
        // TODO SERVER-104131 abort for transaction.
        return {};
    }

    auto shardResponse = response.swResponse.getValue();
    LOGV2_DEBUG(10347003,
                4,
                "Processing cluster write shard response",
                "response"_attr = shardResponse.data,
                "host"_attr = shardResponse.target);

    // Handle any top level errors.
    const auto& ops = response.ops;
    auto shardResponseStatus = getStatusFromCommandResult(shardResponse.data);
    if (!shardResponseStatus.isOK()) {
        for (const auto& op : ops) {
            auto [it, _] =
                _results.emplace(op.getId(), BulkWriteReplyItem(op.getId(), shardResponseStatus));
            it->second.setIdx(op.getId());
        }
        _nErrors += ops.size();
        auto status = shardResponseStatus.withContext(
            str::stream() << "cluster write results unavailable from " << shardResponse.target);
        LOGV2_DEBUG(10347001,
                    4,
                    "Unable to receive cluster write results from shard",
                    "host"_attr = shardResponse.target);
        return {};
    }

    // Parse and handle inner ok and error responses.
    auto parsedReply = BulkWriteCommandReply::parse(
        IDLParserContext("BulkWriteCommandReply_UnifiedWriteExec"), shardResponse.data);

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
    // TODO SERVER-104115 retried stmts.
    // TODO SERVER-104535 cursor support for UnifiedWriteExec.
    const auto& replyItems = parsedReply.getCursor().getFirstBatch();
    auto result = processOpsInReplyItems(routingCtx, ops, replyItems);
    if (!result.unrecoverableError) {
        result.opsToRetry =
            processOpsNotInReplyItems(ops, replyItems, std::move(result.opsToRetry));
    }
    return result;
}

Result WriteBatchResponseProcessor::processOpsInReplyItems(
    RoutingContext& routingCtx,
    const std::vector<WriteOp>& ops,
    const std::vector<BulkWriteReplyItem>& replyItems) {
    std::vector<WriteOp> toRetry;
    CollectionsToCreate collectionsToCreate;
    bool unrecoverableError = false;
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
            routingCtx.onStaleError(op.getNss(), item.getStatus());
            toRetry.push_back(op);
        } else {
            if (!item.getStatus().isOK()) {
                _nErrors++;
                if (op.getCommand().getOrdered()) {
                    unrecoverableError = true;
                }
            }
            auto [it, _] = _results.emplace(op.getId(), item);
            it->second.setIdx(op.getId());
        }
    }
    return {unrecoverableError, std::move(toRetry), collectionsToCreate};
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

WriteCommandResponse WriteBatchResponseProcessor::generateClientResponse() {
    return _context.visitRequest(OverloadedVisitor{
        [&](const BatchedCommandRequest&) {
            return WriteCommandResponse{generateClientResponseForBatchedCommand()};
        },
        [&](const BulkWriteCommandRequest&) {
            return WriteCommandResponse{generateClientResponseForBulkWriteCommand()};
        }});
}

BulkWriteCommandReply WriteBatchResponseProcessor::generateClientResponseForBulkWriteCommand() {
    std::vector<BulkWriteReplyItem> results;
    for (const auto& [id, item] : _results) {
        results.push_back(item);
        // Set the Idx to be the one from the original client request.
        tassert(10347002,
                fmt::format(
                    "expected id in reply ({}) to match id of operation from original request ({})",
                    item.getIdx(),
                    id),
                static_cast<WriteOpId>(item.getIdx()) == id);
        // TODO SERVER-104123 Handle multi: true case where we have multiple reply items for the
        // same op id from the original client request.
    }
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

    // Aggregate all the write concern errors from the shards.
    if (auto totalWcError = mergeWriteConcernErrors(_wcErrors); totalWcError) {
        resp.setWriteConcernError(new WriteConcernErrorDetail{totalWcError->toStatus()});
    }

    return resp;
}

}  // namespace mongo::unified_write_executor
