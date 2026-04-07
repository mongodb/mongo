/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/generic_argument_util.h"
#include "mongo/db/operation_context.h"
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
                           bool setWriteConcern = true) {
    if (cmd.getDbName().isEmpty()) {
        cmd.setDbName(DatabaseName::kAdmin);
    }
    if (resharding::gFeatureFlagReshardingInitNoRefresh.isEnabled(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        generic_argument_util::setOperationSessionInfo(cmd, osi);
    }
    if (setWriteConcern) {
        generic_argument_util::setMajorityWriteConcern(cmd, &resharding::kMajorityWriteConcern);
    }
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<Cmd>>(**executor, token, cmd);
    resharding::sendCommandToShards(opCtx, opts, shardIds);
}

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
