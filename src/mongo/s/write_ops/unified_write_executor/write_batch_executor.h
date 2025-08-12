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

#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/request_types/cluster_commands_without_shard_key_gen.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_batcher.h"

#include <boost/optional.hpp>

namespace mongo {
namespace unified_write_executor {

struct ShardResponse {
    StatusWith<executor::RemoteCommandResponse> swResponse;
    // Ops referencing the original command from the client in the order they were specified in the
    // command to the shard. The items in this array should appear in the order you would see them
    // in the reply item's of a bulk write so that response.ops[replyItem.getIdx()] shound return
    // the corresponding WriteOp from the original command from the client.
    std::vector<WriteOp> ops;
};

using SimpleWriteBatchResponse = std::map<ShardId, ShardResponse>;

struct NonTargetedWriteBatchResponse {
    StatusWith<ClusterWriteWithoutShardKeyResponse> swResponse;
    boost::optional<WriteConcernErrorDetail> wce;
    WriteOp op;
};

using WriteBatchResponse = std::variant<SimpleWriteBatchResponse, NonTargetedWriteBatchResponse>;

class WriteBatchExecutor {
public:
    WriteBatchExecutor(WriteCommandRef cmdRef) : _cmdRef(cmdRef) {}

    WriteBatchExecutor(const BatchedCommandRequest& request)
        : WriteBatchExecutor(WriteCommandRef{request}) {}

    WriteBatchExecutor(const BulkWriteCommandRequest& request)
        : WriteBatchExecutor(WriteCommandRef{request}) {}

    WriteBatchResponse execute(OperationContext* opCtx,
                               RoutingContext& routingCtx,
                               const WriteBatch& batch);

private:
    WriteBatchResponse _execute(OperationContext* opCtx,
                                RoutingContext& routingCtx,
                                const SimpleWriteBatch& batch);

    WriteBatchResponse _execute(OperationContext* opCtx,
                                RoutingContext& routingCtx,
                                const NonTargetedWriteBatch& batch);

    BSONObj buildBulkWriteRequest(
        OperationContext* opCtx,
        const std::vector<WriteOp>& ops,
        const std::map<NamespaceString, ShardEndpoint>& versionByNss,
        bool shouldAppendLsidAndTxnNumber,
        bool shouldAppendWriteConcern,
        boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery = boost::none) const;

    const WriteCommandRef _cmdRef;
};

}  // namespace unified_write_executor
}  // namespace mongo
