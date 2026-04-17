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

#include "mongo/db/s/resharding/resharding_coordinator_command_util.h"

#include "mongo/db/global_catalog/ddl/shardsvr_join_migrations_request_gen.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/shardsvr_resharding_commands_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/s/request_types/abort_reshard_collection_gen.h"
#include "mongo/s/request_types/commit_reshard_collection_gen.h"

namespace mongo {
namespace {
std::vector<ShardId> getAllParticipantShardIds(const ReshardingCoordinatorDocument& doc) {
    auto donorShardIds = resharding::extractShardIdsFromParticipantEntries(doc.getDonorShards());
    auto recipientShardIds =
        resharding::extractShardIdsFromParticipantEntries(doc.getRecipientShards());

    std::set<ShardId> shardIds{donorShardIds.begin(), donorShardIds.end()};
    shardIds.insert(recipientShardIds.begin(), recipientShardIds.end());
    return {shardIds.begin(), shardIds.end()};
}
}  // namespace

namespace resharding {

void tellAllParticipantsToJoinMigrations(
    OperationContext* opCtx,
    const OperationSessionInfo& osi,
    const ReshardingCoordinatorDocument& doc,
    CancellationToken stepdownToken,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    ShardsvrJoinMigrations joinMigrationsCmd;
    sendReshardingCommand(
        opCtx, osi, joinMigrationsCmd, stepdownToken, executor, getAllParticipantShardIds(doc));
}

void tellAllParticipantsToCommit(OperationContext* opCtx,
                                 const OperationSessionInfo& osi,
                                 const ReshardingCoordinatorDocument& doc,
                                 CancellationToken stepdownToken,
                                 const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    ShardsvrCommitReshardCollection cmd(doc.getSourceNss());
    cmd.setReshardingUUID(doc.getReshardingUUID());

    sendReshardingCommand(opCtx, osi, cmd, stepdownToken, executor, getAllParticipantShardIds(doc));
}

void tellAllParticipantsToAbort(OperationContext* opCtx,
                                const OperationSessionInfo& osi,
                                const ReshardingCoordinatorDocument& doc,
                                CancellationToken stepdownToken,
                                const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                                bool isUserAborted) {
    ShardsvrAbortReshardCollection abortCmd(doc.getReshardingUUID(), isUserAborted);

    sendReshardingCommand(
        opCtx, osi, abortCmd, stepdownToken, executor, getAllParticipantShardIds(doc));
}

void tellAllDonorsToInitialize(OperationContext* opCtx,
                               const OperationSessionInfo& osi,
                               const ReshardingCoordinatorDocument& doc,
                               CancellationToken stepdownToken,
                               const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    ShardsvrReshardDonorInitialize cmd(doc.getReshardingUUID());
    cmd.setCommonReshardingMetadata(doc.getCommonReshardingMetadata());
    cmd.setRecipientShards(
        resharding::extractShardIdsFromParticipantEntries(doc.getRecipientShards()));

    sendReshardingCommand(opCtx,
                          osi,
                          cmd,
                          stepdownToken,
                          executor,
                          resharding::extractShardIdsFromParticipantEntries(doc.getDonorShards()));
}

void tellAllRecipientsToInitialize(OperationContext* opCtx,
                                   const OperationSessionInfo& osi,
                                   const ReshardingCoordinatorDocument& doc,
                                   CancellationToken stepdownToken,
                                   const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    std::vector<DonorShardFetchTimestamp> donorShards;
    for (const auto& donor : doc.getDonorShards()) {
        DonorShardFetchTimestamp donorFetchTimestamp(donor.getId());
        donorShards.push_back(std::move(donorFetchTimestamp));
    }

    ReshardingRecipientOptions recipientOptions(
        std::move(donorShards),
        doc.getDemoMode() ? 0 : resharding::gReshardingMinimumOperationDurationMillis.load());
    recipientOptions.setRelaxed(doc.getRelaxed());

    ShardsvrReshardRecipientInitialize cmd(doc.getReshardingUUID());
    cmd.setCommonReshardingMetadata(doc.getCommonReshardingMetadata());
    cmd.setRecipientOptions(std::move(recipientOptions));

    sendReshardingCommand(
        opCtx,
        osi,
        cmd,
        stepdownToken,
        executor,
        resharding::extractShardIdsFromParticipantEntries(doc.getRecipientShards()));
}

void tellAllDonorsToStartChangeStreamsMonitor(
    OperationContext* opCtx,
    const OperationSessionInfo& osi,
    const ReshardingCoordinatorDocument& doc,
    CancellationToken stepdownToken,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    invariant(doc.getCloneTimestamp());
    ShardsvrReshardingDonorStartChangeStreamsMonitor cmd(
        doc.getSourceNss(), doc.getReshardingUUID(), *doc.getCloneTimestamp());

    // The donors ensure the change streams monitor start time is majority committed in their
    // state document before returning, so no write concern is needed.
    sendReshardingCommand(opCtx,
                          osi,
                          cmd,
                          stepdownToken,
                          executor,
                          resharding::extractShardIdsFromParticipantEntries(doc.getDonorShards()),
                          false /* setWriteConcern */);
}

void tellAllRecipientsToClone(OperationContext* opCtx,
                              const OperationSessionInfo& osi,
                              const ReshardingCoordinatorDocument& doc,
                              CancellationToken stepdownToken,
                              const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto [shardsOwningChunks, shardsNotOwningChunks] =
        resharding::computeRecipientChunkOwnership(opCtx, doc);

    auto recipientFields = resharding::constructRecipientFields(doc);
    ShardsvrReshardRecipientClone cmd(doc.getReshardingUUID());
    cmd.setCloneTimestamp(recipientFields.getCloneTimestamp().get());
    cmd.setDonorShards(recipientFields.getDonorShards());
    cmd.setApproxCopySize(recipientFields.getReshardingApproxCopySizeStruct());

    sendReshardingCommand(opCtx,
                          osi,
                          cmd,
                          stepdownToken,
                          executor,
                          {shardsOwningChunks.begin(), shardsOwningChunks.end()});

    if (!shardsNotOwningChunks.empty()) {
        ReshardingApproxCopySize approxCopySize;
        approxCopySize.setApproxBytesToCopy(0);
        approxCopySize.setApproxDocumentsToCopy(0);
        cmd.setApproxCopySize(approxCopySize);

        sendReshardingCommand(opCtx,
                              osi,
                              cmd,
                              stepdownToken,
                              executor,
                              {shardsNotOwningChunks.begin(), shardsNotOwningChunks.end()});
    }
}

void tellAllRecipientsCriticalSectionStarted(
    OperationContext* opCtx,
    const OperationSessionInfo& osi,
    const ReshardingCoordinatorDocument& doc,
    CancellationToken abortToken,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    ShardsvrReshardRecipientCriticalSectionStarted cmd(doc.getReshardingUUID());

    sendReshardingCommand(
        opCtx,
        osi,
        cmd,
        abortToken,
        executor,
        resharding::extractShardIdsFromParticipantEntries(doc.getRecipientShards()));
}

}  // namespace resharding
}  // namespace mongo
