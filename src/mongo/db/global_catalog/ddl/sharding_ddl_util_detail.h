/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/routing_context.h"
#include "mongo/db/topology/user_write_block/write_block_bypass.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/executor/async_rpc_error_info.h"
#include "mongo/executor/async_rpc_targeter.h"
#include "mongo/executor/async_rpc_util.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/metadata/audit_metadata.h"
#include "mongo/s/async_requests_sender.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace mongo {
namespace MONGO_MOD_PRIVATE sharding_ddl_util_detail {

template <typename CommandType>
void preprocessCommand(OperationContext* opCtx, CommandType& cmd) {
    // AsyncRPC ignores audit metadata so we need to manually attach them to the command
    if (auto meta = rpc::getAuditAttrsToAuditMetadata(opCtx)) {
        cmd.setDollarAudit(*meta);
    }
    // TODO SERVER-99655: hasOperationFCV() will always be true once DDL coordinators always use
    // OFCV
    if (auto& vCtx = VersionContext::getDecoration(opCtx); vCtx.hasOperationFCV()) {
        tassert(11144300,
                "Expected VersionContext with propagation across shards",
                vCtx.canPropagateAcrossShards());
        cmd.setVersionContext(vCtx);
    }
    cmd.setMayBypassWriteBlocking(WriteBlockBypass::get(opCtx).isWriteBlockBypassEnabled());
}

template <typename CommandType, typename GetShardId, typename BuildFutures>
auto sendCommands(OperationContext* opCtx,
                  async_rpc::AsyncRPCOptions<CommandType>* originalOpts,
                  bool throwOnError,
                  GetShardId&& getShardId,
                  BuildFutures&& buildFutures) {

    std::vector<ExecutorFuture<async_rpc::AsyncRPCResponse<typename CommandType::Reply>>> futures;

    CancellationSource cancelSource(originalOpts->token);
    try {
        buildFutures(cancelSource, futures);
    } catch (const DBException&) {
        // In case of exception, we might have already sent some commands.
        // Before returning the exception to the caller, we have to cancel the scheduled async
        // operations and wait for them to drain or we might access invalid pointers.
        cancelSource.cancel();

        // Wait for all futures to cancel, discarding results and errors
        for (auto&& future : futures) {
            auto _ = std::move(future).getNoThrow();
        }
        throw;
    }

    auto formatResponse = [](async_rpc::AsyncRPCResponse<typename CommandType::Reply> reply) {
        if constexpr (std::is_void_v<typename CommandType::Reply>) {
            return executor::RemoteCommandResponse(reply.targetUsed, BSONObj{}, reply.elapsed);
        } else {
            BSONObjBuilder replyBob;
            reply.response.serialize(&replyBob);
            reply.genericReplyFields.serialize(&replyBob);
            return executor::RemoteCommandResponse(reply.targetUsed, replyBob.obj(), reply.elapsed);
        }
    };

    if (throwOnError) {
        auto responses =
            async_rpc::getAllResponsesOrFirstErrorWithCancellation<
                AsyncRequestsSender::Response,
                async_rpc::AsyncRPCResponse<typename CommandType::Reply>>(
                std::move(futures),
                cancelSource,
                [&getShardId,
                 formatResponse](async_rpc::AsyncRPCResponse<typename CommandType::Reply> reply,
                                 size_t index) -> AsyncRequestsSender::Response {
                    return AsyncRequestsSender::Response{getShardId(index), formatResponse(reply)};
                })
                .getNoThrow();

        if (auto status = responses.getStatus(); status != Status::OK()) {
            uassertStatusOK(async_rpc::unpackRPCStatus(status));
        }

        return responses.getValue();
    } else {
        return async_rpc::getAllResponses<AsyncRequestsSender::Response,
                                          async_rpc::AsyncRPCResponse<typename CommandType::Reply>>(
                   std::move(futures),
                   [&getShardId, formatResponse](
                       StatusWith<async_rpc::AsyncRPCResponse<typename CommandType::Reply>> swReply,
                       size_t index) -> AsyncRequestsSender::Response {
                       auto response = [&]() -> StatusWith<executor::RemoteCommandResponse> {
                           if (!swReply.isOK()) {
                               return async_rpc::unpackRPCStatus(swReply.getStatus());
                           } else {
                               return formatResponse(swReply.getValue());
                           }
                       }();

                       return AsyncRequestsSender::Response{getShardId(index), response};
                   })
            .get();
    }
}

template <typename CommandType>
std::vector<AsyncRequestsSender::Response> sendAuthenticatedVersionedCommandTargetedByRoutingTable(
    OperationContext* opCtx,
    std::shared_ptr<async_rpc::AsyncRPCOptions<CommandType>> originalOpts,
    RoutingContext& routingCtx,
    const NamespaceString& nss,
    ReadPreferenceSetting readPref,
    bool throwOnError) {

    preprocessCommand(opCtx, originalOpts->cmd);

    auto requests = buildVersionedCommandsByRoutingTable(opCtx, nss, routingCtx, originalOpts->cmd);
    routingCtx.onRequestSentForNss(nss);

    return sendCommands(
        opCtx,
        originalOpts.get(),
        throwOnError,
        [&](int index) { return requests[index].first; },
        [&](const CancellationSource& cancelSource, auto& futures) {
            for (auto& [shardId, request] : requests) {
                std::unique_ptr<async_rpc::Targeter> targeter =
                    std::make_unique<async_rpc::ShardIdTargeter>(
                        originalOpts->exec, opCtx, shardId, readPref);
                auto shard =
                    uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));
                Shard::RetryStrategy::RequestStartTransactionState startTxnState =
                    originalOpts->cmd.getStartTransaction().value_or(false)
                    ? Shard::RetryStrategy::RequestStartTransactionState::kStartingTransaction
                    : Shard::RetryStrategy::RequestStartTransactionState::kNotStartingTransaction;
                auto retryStrategy = std::make_shared<Shard::OwnerRetryStrategy>(
                    shard, Shard::RetryPolicy::kIdempotentOrCursorInvalidated, startTxnState);
                auto opts = std::make_shared<async_rpc::AsyncRPCOptions<CommandType>>(
                    originalOpts->exec, cancelSource.token(), std::move(request), retryStrategy);
                futures.push_back(
                    async_rpc::sendCommand<CommandType>(opts, opCtx, std::move(targeter)));
            }
        });
}


template <typename CommandType>
std::vector<AsyncRequestsSender::Response> sendAuthenticatedCommandToShards(
    OperationContext* opCtx,
    std::shared_ptr<async_rpc::AsyncRPCOptions<CommandType>> originalOpts,
    const std::vector<ShardId>& shardIds,
    const boost::optional<std::vector<ShardVersion>>& shardVersions,
    ReadPreferenceSetting readPref,
    bool throwOnError) {
    if (shardIds.size() == 0) {
        return {};
    }
    if (shardVersions) {
        tassert(11532300,
                "shardIds and shardVersions must have the same size",
                shardIds.size() == shardVersions->size());
    }

    preprocessCommand(opCtx, originalOpts->cmd);

    return sendCommands(
        opCtx,
        originalOpts.get(),
        throwOnError,
        [&](int index) { return shardIds[index]; },
        [&](const CancellationSource& cancelSource, auto& futures) {
            for (size_t i = 0; i < shardIds.size(); ++i) {
                std::unique_ptr<async_rpc::Targeter> targeter =
                    std::make_unique<async_rpc::ShardIdTargeter>(
                        originalOpts->exec, opCtx, shardIds[i], readPref);
                auto shard = uassertStatusOK(
                    Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardIds[i]));
                Shard::RetryStrategy::RequestStartTransactionState startTxnState =
                    originalOpts->cmd.getStartTransaction().value_or(false)
                    ? Shard::RetryStrategy::RequestStartTransactionState::kStartingTransaction
                    : Shard::RetryStrategy::RequestStartTransactionState::kNotStartingTransaction;
                auto retryStrategy = std::make_shared<Shard::OwnerRetryStrategy>(
                    shard, Shard::RetryPolicy::kIdempotentOrCursorInvalidated, startTxnState);

                auto opts = std::make_shared<async_rpc::AsyncRPCOptions<CommandType>>(
                    originalOpts->exec, cancelSource.token(), originalOpts->cmd, retryStrategy);
                if (shardVersions) {
                    opts->cmd.setShardVersion((*shardVersions)[i]);
                }
                futures.push_back(
                    async_rpc::sendCommand<CommandType>(opts, opCtx, std::move(targeter)));
            }
        });
}

// `sendAuthenticatedCommandToShards` instantiation uses a lot of memory. Make the template
// instantiation explicit, so that we can offload them to a different translation unit.
// These three instantiations in particular are used in `sharding_ddl_util.cpp`.

extern template std::vector<AsyncRequestsSender::Response>
sendAuthenticatedCommandToShards<write_ops::UpdateCommandRequest>(
    OperationContext* opCtx,
    std::shared_ptr<async_rpc::AsyncRPCOptions<write_ops::UpdateCommandRequest>> originalOpts,
    const std::vector<ShardId>& shardIds,
    const boost::optional<std::vector<ShardVersion>>& shardVersions,
    ReadPreferenceSetting readPref,
    bool throwOnError);

extern template std::vector<AsyncRequestsSender::Response>
sendAuthenticatedCommandToShards<ShardsvrDropCollectionParticipant>(
    OperationContext* opCtx,
    std::shared_ptr<async_rpc::AsyncRPCOptions<ShardsvrDropCollectionParticipant>> originalOpts,
    const std::vector<ShardId>& shardIds,
    const boost::optional<std::vector<ShardVersion>>& shardVersions,
    ReadPreferenceSetting readPref,
    bool throwOnError);

extern template std::vector<AsyncRequestsSender::Response>
sendAuthenticatedCommandToShards<ShardsvrCommitCreateDatabaseMetadata>(
    OperationContext* opCtx,
    std::shared_ptr<async_rpc::AsyncRPCOptions<ShardsvrCommitCreateDatabaseMetadata>> originalOpts,
    const std::vector<ShardId>& shardIds,
    const boost::optional<std::vector<ShardVersion>>& shardVersions,
    ReadPreferenceSetting readPref,
    bool throwOnError);

extern template std::vector<AsyncRequestsSender::Response>
sendAuthenticatedCommandToShards<ShardsvrCommitDropDatabaseMetadata>(
    OperationContext* opCtx,
    std::shared_ptr<async_rpc::AsyncRPCOptions<ShardsvrCommitDropDatabaseMetadata>> originalOpts,
    const std::vector<ShardId>& shardIds,
    const boost::optional<std::vector<ShardVersion>>& shardVersions,
    ReadPreferenceSetting readPref,
    bool throwOnError);

}  // namespace MONGO_MOD_PRIVATE sharding_ddl_util_detail
}  // namespace mongo
