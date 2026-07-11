// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_coordinator_command_util.h"

#include "mongo/db/global_catalog/ddl/shardsvr_join_migrations_request_gen.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/shardsvr_resharding_commands_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/s/request_types/abort_reshard_collection_gen.h"
#include "mongo/s/request_types/commit_reshard_collection_gen.h"

namespace mongo {
namespace resharding {

std::vector<ShardId> getAllParticipantShardIds(const ReshardingCoordinatorDocument& doc) {
    auto donorShardIds = extractShardIdsFromParticipantEntries(doc.getDonorShards());
    auto recipientShardIds = extractShardIdsFromParticipantEntries(doc.getRecipientShards());

    std::set<ShardId> shardIds{donorShardIds.begin(), donorShardIds.end()};
    shardIds.insert(recipientShardIds.begin(), recipientShardIds.end());
    return {shardIds.begin(), shardIds.end()};
}

void tellAllParticipantsToJoinMigrations(
    OperationContext* opCtx,
    const OperationSessionInfo& osi,
    const ReshardingCoordinatorDocument& doc,
    CancellationToken stepdownToken,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    ShardsvrJoinMigrations joinMigrationsCmd;
    sendReshardingCommand(opCtx,
                          osi,
                          joinMigrationsCmd,
                          stepdownToken,
                          executor,
                          getAllParticipantShardIds(doc),
                          doc.getCommonReshardingMetadata().getForwardableOpMetadata());
}

void tellAllShardsToCleanupStaleChunks(
    OperationContext* opCtx,
    const OperationSessionInfo& osi,
    const std::vector<ShardId>& shardIds,
    const NamespaceString& nss,
    const UUID& oldUUID,
    CancellationToken stepdownToken,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    boost::optional<ForwardableOperationMetadata> fom) {
    ShardsvrReshardCleanupStaleChunks cmd(nss);
    cmd.setOldUUID(oldUUID);

    sendReshardingCommand(opCtx, osi, cmd, stepdownToken, executor, shardIds, fom);
}

void tellAllParticipantsToCommit(OperationContext* opCtx,
                                 const OperationSessionInfo& osi,
                                 const ReshardingCoordinatorDocument& doc,
                                 CancellationToken stepdownToken,
                                 const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    ShardsvrCommitReshardCollection cmd(doc.getSourceNss());
    cmd.setReshardingUUID(doc.getReshardingUUID());

    sendReshardingCommand(opCtx,
                          osi,
                          cmd,
                          stepdownToken,
                          executor,
                          getAllParticipantShardIds(doc),
                          doc.getCommonReshardingMetadata().getForwardableOpMetadata());
}

void tellAllParticipantsToAbort(OperationContext* opCtx,
                                const OperationSessionInfo& osi,
                                const ReshardingCoordinatorDocument& doc,
                                CancellationToken stepdownToken,
                                const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                                bool isUserAborted) {
    ShardsvrAbortReshardCollection abortCmd(doc.getReshardingUUID(), isUserAborted);

    sendReshardingCommand(opCtx,
                          osi,
                          abortCmd,
                          stepdownToken,
                          executor,
                          getAllParticipantShardIds(doc),
                          doc.getCommonReshardingMetadata().getForwardableOpMetadata());
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
                          resharding::extractShardIdsFromParticipantEntries(doc.getDonorShards()),
                          doc.getCommonReshardingMetadata().getForwardableOpMetadata());
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
        resharding::extractShardIdsFromParticipantEntries(doc.getRecipientShards()),
        doc.getCommonReshardingMetadata().getForwardableOpMetadata());
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
                          doc.getCommonReshardingMetadata().getForwardableOpMetadata(),
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
                          {shardsOwningChunks.begin(), shardsOwningChunks.end()},
                          doc.getCommonReshardingMetadata().getForwardableOpMetadata());

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
                              {shardsNotOwningChunks.begin(), shardsNotOwningChunks.end()},
                              doc.getCommonReshardingMetadata().getForwardableOpMetadata());
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
        resharding::extractShardIdsFromParticipantEntries(doc.getRecipientShards()),
        doc.getCommonReshardingMetadata().getForwardableOpMetadata());
}

}  // namespace resharding
}  // namespace mongo
