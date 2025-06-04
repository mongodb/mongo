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

namespace mongo::unified_write_executor {
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
 * // Generate a response based on the original command we recieved.
 * auto = generateClientResponse<CmdResponse>()
 */
class WriteBatchResponseProcessor {
public:
    using CollectionsToCreate =
        stdx::unordered_map<NamespaceString,
                            std::shared_ptr<const mongo::CannotImplicitlyCreateCollectionInfo>>;
    /**
     * A pair representing ops to be retried and collections to create.
     */
    using Result = std::pair<std::vector<WriteOp>, CollectionsToCreate>;
    /**
     * Process a response from each shard, handle errors, and collect statistics. Returns an
     * array containing ops that did not complete successfully that need to be resent.
     */
    Result onWriteBatchResponse(const WriteBatchResponse& response);

    /**
     * Turns gathered statistics into a command reply for the client. Consumes any pending reply
     * items.
     */
    template <typename CmdResponse>
    CmdResponse generateClientResponse();

private:
    /**
     * Process a response from a shard, handle errors, and collect statistics. Returns an array
     * containing ops that did not complete successfully that need to be resent.
     */
    Result onShardResponse(const ShardResponse& response);

    /**
     * Process ReplyItems and pick out any ops that need to be retried.
     */
    Result processOpsInReplyItems(const std::vector<WriteOp>& ops,
                                  const std::vector<BulkWriteReplyItem>&);
    /**
     * If an op was not in the ReplyItems, this function processes it and decides if a retry is
     * needed.
     */
    std::vector<WriteOp> processOpsNotInReplyItems(const std::vector<WriteOp>& requestedOps,
                                                   const std::vector<BulkWriteReplyItem>&,
                                                   std::vector<WriteOp>&& toRetry);

    size_t _nErrors{0};
    size_t _nInserted{0};
    size_t _nMatched{0};
    size_t _nModified{0};
    size_t _nUpserted{0};
    size_t _nDeleted{0};
    std::map<WriteOpId, BulkWriteReplyItem> _results;
};

}  // namespace mongo::unified_write_executor
