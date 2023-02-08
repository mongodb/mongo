/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/s/write_ops/bulk_write_exec.h"

#include "mongo/base/error_codes.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace bulkWriteExec {

void execute(OperationContext* opCtx,
             const std::vector<std::unique_ptr<NSTargeter>>& targeters,
             const BulkWriteCommandRequest& clientRequest,
             BulkWriteCommandReply* reply) {
    LOGV2_DEBUG(7263700,
                4,
                "Starting execution of a bulkWrite",
                "size"_attr = clientRequest.getOps().size(),
                "nsInfoSize"_attr = clientRequest.getNsInfo().size());

    BulkWriteOp bulkWriteOp(opCtx, clientRequest);

    while (!bulkWriteOp.isFinished()) {
        // 1: Target remaining ops with the appropriate targeter based on the namespace index and
        // re-batch ops based on their targeted shard id.
        stdx::unordered_map<ShardId, std::unique_ptr<TargetedWriteBatch>> childBatches;
        auto targetStatus = bulkWriteOp.target(targeters, &childBatches);

        // 2: Use MultiStatementTransactionRequestsSender to send any ready sub-batches to targeted
        // shard endpoints.

        // 3: Wait for responses for all those sub-batches and keep track of the responses from
        // sub-batches based on the op index in the original bulkWrite command. Abort the batch upon
        // errors for ordered writes or transactions.

        // 4: Refresh the targeter(s) if we receive a stale config/db error.
    }

    // Reassemble the final response based on responses from sub-batches.
    auto replies = std::vector<BulkWriteReplyItem>();
    replies.emplace_back(0);
    reply->setCursor(BulkWriteCommandResponseCursor(0, replies));

    LOGV2_DEBUG(7263701, 4, "Finished execution of bulkWrite");
    return;
}

BulkWriteOp::BulkWriteOp(OperationContext* opCtx, const BulkWriteCommandRequest& clientRequest)
    : _opCtx(opCtx),
      _clientRequest(clientRequest),
      _txnNum(_opCtx->getTxnNumber()),
      _inTransaction(static_cast<bool>(TransactionRouter::get(opCtx))),
      _isRetryableWrite(opCtx->isRetryableWrite()) {
    _writeOps.reserve(_clientRequest.getOps().size());
    for (size_t i = 0; i < _clientRequest.getOps().size(); ++i) {
        _writeOps.emplace_back(BatchItemRef(&_clientRequest, i), _inTransaction);
    }
}

StatusWith<bool> BulkWriteOp::target(
    const std::vector<std::unique_ptr<NSTargeter>>& targeters,
    stdx::unordered_map<ShardId, std::unique_ptr<TargetedWriteBatch>>* targetedBatches) {
    return false;
}

bool BulkWriteOp::isFinished() {
    // TODO: Track ops lifetime.
    return true;
}
}  // namespace bulkWriteExec

}  // namespace mongo
