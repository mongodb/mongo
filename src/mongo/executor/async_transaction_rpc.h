// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/s/async_rpc_shard_targeter.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/modules.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kExecutor
namespace mongo::async_rpc {
/**
 * This function operates in the same way as `async_rpc::sendCommand`, but will attach transaction
 * metadata from the opCtx to the command BSONObject metadata before sending to the targeted
 * shardId.
 */
template <typename CommandType>
[[MONGO_MOD_PUBLIC]] ExecutorFuture<AsyncRPCResponse<typename CommandType::Reply>> sendTxnCommand(
    std::shared_ptr<AsyncRPCOptions<CommandType>> options,
    OperationContext* opCtx,
    std::unique_ptr<ShardIdTargeter> targeter) {
    using ReplyType = AsyncRPCResponse<typename CommandType::Reply>;
    // Execute the command after extracting the db name and bson from the CommandType.
    // Wrapping this function allows us to separate the CommandType parsing logic from the
    // implementation details of executing the remote command asynchronously.
    auto runner = detail::AsyncRPCRunner::get(opCtx->getServiceContext());
    auto cmdBSON = options->cmd.toBSON();
    const auto shardId = targeter->getShardId();
    if (auto txnRouter = TransactionRouter::get(opCtx); txnRouter) {
        cmdBSON = txnRouter.attachTxnFieldsIfNeeded(opCtx, targeter->getShardId(), cmdBSON);
    }
    return detail::sendCommandWithRunner(options, opCtx, runner, std::move(targeter), cmdBSON)
        .onCompletion([opCtx, shardId](StatusWith<ReplyType> swResponse) -> StatusWith<ReplyType> {
            auto txnRouter = TransactionRouter::get(opCtx);
            if (!txnRouter) {
                return swResponse;
            }
            if (swResponse.isOK()) {
                ReplyType reply = swResponse.getValue();
                const GenericReplyFields& gens = reply.genericReplyFields;

                // Extract the transaction-related metadata from the response.
                // TODO SERVER-106098: remove this implicit coupling between this file and the
                // 'TxnResponseMetadata'. It would be better if 'TransactionRouter' provided a
                // function that creates a 'TxnResponseMetadata' object from the generic reply
                // fields, so that all knowledge about what makes a 'TxnResponseMetadata' stays
                // within the 'TransactionRouter'.
                TxnResponseMetadata txnResponseMetadata;
                txnResponseMetadata.setReadOnly(gens.getReadOnly());

                // If the response contains additional transaction participants, we need to convert
                // them from their BSON representation to the internal representation first.
                if (const auto& additionalParticipants = gens.getAdditionalParticipants()) {
                    if (!additionalParticipants->empty()) {
                        std::vector<AdditionalParticipantInfo> converted;
                        std::transform(
                            additionalParticipants->begin(),
                            additionalParticipants->end(),
                            std::back_inserter(converted),
                            [](const BSONObj& participantBSON) {
                                return AdditionalParticipantInfo::parse(
                                    participantBSON,
                                    IDLParserContext("AdditionalTransactionParticipant"));
                            });
                        txnResponseMetadata.setAdditionalParticipants(std::move(converted));
                    }
                }

                // We only get here after the API has already unrolled the response object and has
                // verified that the response has an OK status. That means we can assume successful
                // execution of the RPC, and can use 'Status::OK()' as the only possible status when
                // constructing the additional participant metadata below.
                txnRouter.processParticipantResponse(
                    opCtx,
                    shardId,
                    {.status = Status::OK(),
                     .txnResponseMetadata = std::move(txnResponseMetadata)});
            } else {
                auto extraInfo = swResponse.getStatus().template extraInfo<AsyncRPCErrorInfo>();
                if (extraInfo->isRemote()) {
                    auto remoteError = extraInfo->asRemote();
                    txnRouter.processParticipantResponse(
                        opCtx,
                        shardId,
                        TransactionRouter::Router::parseParticipantResponseMetadata(
                            remoteError.getResponseObj()));
                }
            }
            return swResponse;
        })
        .thenRunOn(options->exec);
}
}  // namespace mongo::async_rpc
#undef MONGO_LOGV2_DEFAULT_COMPONENT
