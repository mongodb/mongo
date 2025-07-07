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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::unified_write_executor {
using Result = WriteBatchResponseProcessor::Result;

Result WriteBatchResponseProcessor::onWriteBatchResponse(const WriteBatchResponse& response) {
    std::vector<WriteOp> toRetry;
    CollectionsToCreate toCreate;
    for (const auto& [_, shardResponse] : response) {
        auto [ops, collectionsToCreate] = onShardResponse(shardResponse);
        toRetry.insert(toRetry.end(),
                       std::make_move_iterator(ops.begin()),
                       std::make_move_iterator(ops.end()));
        for (auto& [nss, info] : collectionsToCreate) {
            if (auto it = toCreate.find(nss); it == toCreate.cend()) {
                toCreate.emplace(nss, std::move(info));
            }
        }
    }
    return {std::move(toRetry), std::move(toCreate)};
}

Result WriteBatchResponseProcessor::onShardResponse(const ShardResponse& response) {
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

    _nInserted += parsedReply.getNInserted();
    _nDeleted += parsedReply.getNDeleted();
    _nMatched += parsedReply.getNMatched();
    _nUpserted += parsedReply.getNUpserted();
    _nModified += parsedReply.getNModified();
    // TODO SERVER-104130 WriteConcern/WriteConcernError support.
    // TODO SERVER-104115 retried stmts.
    // TODO SERVER-104535 cursor support for UnifiedWriteExec.
    const auto& replyItems = parsedReply.getCursor().getFirstBatch();
    auto [toRetry, collectionsToCreate] = processOpsInReplyItems(ops, replyItems);
    return {processOpsNotInReplyItems(ops, replyItems, std::move(toRetry)), collectionsToCreate};
}

Result WriteBatchResponseProcessor::processOpsInReplyItems(
    const std::vector<WriteOp>& ops, const std::vector<BulkWriteReplyItem>& replyItems) {
    std::vector<WriteOp> toRetry;
    CollectionsToCreate collectionsToCreate;
    for (const auto& item : replyItems) {
        // TODO SERVER-104114 support retrying staleness errors.
        // TODO SERVER-104122 Support for 'WouldChangeOwningShard' writes.
        tassert(10347004,
                fmt::format("shard replied with invalid opId {} when it was only sent {} ops",
                            item.getIdx(),
                            ops.size()),
                static_cast<WriteOpId>(item.getIdx()) < ops.size());
        const auto& op = ops[item.getIdx()];

        if (item.getStatus().code() == ErrorCodes::StaleConfig) {
            LOGV2_DEBUG(
                10346900, 4, "Noting stale config response", "status"_attr = item.getStatus());
            toRetry.push_back(op);
        } else if (item.getStatus().code() == ErrorCodes::CannotImplicitlyCreateCollection) {
            // Stage the collection to be created if it was found to not exist.
            auto info = item.getStatus().extraInfo<CannotImplicitlyCreateCollectionInfo>();
            if (auto it = collectionsToCreate.find(info->getNss());
                it == collectionsToCreate.cend()) {
                collectionsToCreate.emplace(info->getNss(), std::move(info));
            }
            toRetry.push_back(op);
        } else {
            if (!item.getStatus().isOK()) {
                _nErrors++;
            }
            auto [it, _] = _results.emplace(op.getId(), item);
            it->second.setIdx(op.getId());
        }
    }
    return {std::move(toRetry), collectionsToCreate};
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
            toRetry.push_back(requestedOps[i]);
        }
    }
    return toRetry;
}

template <>
BulkWriteCommandReply WriteBatchResponseProcessor::generateClientResponse<BulkWriteCommandReply>() {
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
    return BulkWriteCommandReply(
        BulkWriteCommandResponseCursor(
            0, std::move(results), NamespaceString::makeBulkWriteNSS(boost::none)),
        _nErrors,
        _nInserted,
        _nMatched,
        _nModified,
        _nUpserted,
        _nDeleted);
    // TODO SERVER-104535 cursor support for UnifiedWriteExec.
}

}  // namespace mongo::unified_write_executor
