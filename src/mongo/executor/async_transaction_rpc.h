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

#pragma once

#include <memory>

#include "mongo/db/operation_context.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/s/async_rpc_shard_targeter.h"
#include "mongo/s/transaction_router.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kExecutor
namespace mongo::async_rpc {
/**
 * This function operates in the same way as `async_rpc::sendCommand`, but will attach transaction
 * metadata from the opCtx to the command BSONObject metadata before sending to the targeted
 * shardId.
 */
template <typename CommandType>
ExecutorFuture<AsyncRPCResponse<typename CommandType::Reply>> sendTxnCommand(
    std::shared_ptr<AsyncRPCOptions<CommandType>> options,
    OperationContext* opCtx,
    std::unique_ptr<ShardIdTargeter> targeter) {
    using ReplyType = AsyncRPCResponse<typename CommandType::Reply>;
    // Execute the command after extracting the db name and bson from the CommandType.
    // Wrapping this function allows us to separate the CommandType parsing logic from the
    // implementation details of executing the remote command asynchronously.
    auto runner = detail::AsyncRPCRunner::get(opCtx->getServiceContext());
    auto cmdBSON = options->cmd.toBSON({});
    const auto shardId = targeter->getShardId();
    if (auto txnRouter = TransactionRouter::get(opCtx); txnRouter) {
        cmdBSON = txnRouter.attachTxnFieldsIfNeeded(opCtx, targeter->getShardId(), cmdBSON);
    }
    auto genericArgs =
        options->genericArgs.stable.toBSON().addFields(options->genericArgs.unstable.toBSON());
    auto cmdBsonWithArgs = cmdBSON.addFields(genericArgs);
    return detail::sendCommandWithRunner(
               cmdBsonWithArgs, options, runner, opCtx, std::move(targeter))
        .onCompletion([opCtx, shardId](StatusWith<ReplyType> swResponse) -> StatusWith<ReplyType> {
            auto txnRouter = TransactionRouter::get(opCtx);
            if (!txnRouter) {
                return swResponse;
            }
            if (swResponse.isOK()) {
                // TODO (SERVER-72082): Make sure TxnResponseMetadata is appended to the BSON
                // that we are passing into 'processParticipantResponse'.
                txnRouter.processParticipantResponse(
                    opCtx, shardId, swResponse.getValue().response.toBSON());
            } else {
                auto extraInfo = swResponse.getStatus().template extraInfo<AsyncRPCErrorInfo>();
                if (extraInfo->isRemote()) {
                    auto remoteError = extraInfo->asRemote();
                    txnRouter.processParticipantResponse(
                        opCtx, shardId, remoteError.getResponseObj());
                }
            }
            return swResponse;
        })
        .thenRunOn(options->exec);
}
}  // namespace mongo::async_rpc
#undef MONGO_LOGV2_DEFAULT_COMPONENT
