// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/generic_argument_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/resharding/resharding_coordinator.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/cancellation.h"

#include <memory>
#include <vector>

namespace mongo {
namespace resharding {

template <typename Cmd>
void sendReshardingCommand(OperationContext* opCtx,
                           const OperationSessionInfo& osi,
                           Cmd cmd,
                           CancellationToken token,
                           const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                           const std::vector<ShardId>& shardIds,
                           boost::optional<ForwardableOperationMetadata> fom = boost::none,
                           bool setWriteConcern = true) {
    if (cmd.getDbName().isEmpty()) {
        cmd.setDbName(DatabaseName::kAdmin);
    }
    if (resharding::isEnabledWithPinnedVersion(fom,
                                               resharding::gFeatureFlagReshardingInitNoRefresh)) {
        generic_argument_util::setOperationSessionInfo(cmd, osi);
    }
    if (setWriteConcern) {
        generic_argument_util::setMajorityWriteConcern(cmd, &resharding::kMajorityWriteConcern);
    }
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<Cmd>>(**executor, token, cmd);
    resharding::sendCommandToShards(opCtx, opts, shardIds);
}

/**
 * Returns the union of all donor and recipient shard IDs from the coordinator document.
 */
std::vector<ShardId> getAllParticipantShardIds(const ReshardingCoordinatorDocument& doc);

void tellAllParticipantsToJoinMigrations(
    OperationContext* opCtx,
    const OperationSessionInfo& osi,
    const ReshardingCoordinatorDocument& doc,
    CancellationToken stepdownToken,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

void tellAllShardsToCleanupStaleChunks(
    OperationContext* opCtx,
    const OperationSessionInfo& osi,
    const std::vector<ShardId>& shardIds,
    const NamespaceString& nss,
    const UUID& oldUUID,
    CancellationToken stepdownToken,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    boost::optional<ForwardableOperationMetadata> fom = boost::none);

void tellAllParticipantsToCommit(OperationContext* opCtx,
                                 const OperationSessionInfo& osi,
                                 const ReshardingCoordinatorDocument& doc,
                                 CancellationToken stepdownToken,
                                 const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

void tellAllParticipantsToAbort(OperationContext* opCtx,
                                const OperationSessionInfo& osi,
                                const ReshardingCoordinatorDocument& doc,
                                CancellationToken stepdownToken,
                                const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                                bool isUserAborted);

void tellAllDonorsToInitialize(OperationContext* opCtx,
                               const OperationSessionInfo& osi,
                               const ReshardingCoordinatorDocument& doc,
                               CancellationToken stepdownToken,
                               const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

void tellAllRecipientsToInitialize(OperationContext* opCtx,
                                   const OperationSessionInfo& osi,
                                   const ReshardingCoordinatorDocument& doc,
                                   CancellationToken stepdownToken,
                                   const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

void tellAllDonorsToStartChangeStreamsMonitor(
    OperationContext* opCtx,
    const OperationSessionInfo& osi,
    const ReshardingCoordinatorDocument& doc,
    CancellationToken stepdownToken,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

void tellAllRecipientsToClone(OperationContext* opCtx,
                              const OperationSessionInfo& osi,
                              const ReshardingCoordinatorDocument& doc,
                              CancellationToken stepdownToken,
                              const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

void tellAllRecipientsCriticalSectionStarted(
    OperationContext* opCtx,
    const OperationSessionInfo& osi,
    const ReshardingCoordinatorDocument& doc,
    CancellationToken abortToken,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

}  // namespace resharding
}  // namespace mongo
