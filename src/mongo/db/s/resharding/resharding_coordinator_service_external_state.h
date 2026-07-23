// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/primary_only_service_helpers/operation_session_tracker.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/flush_routing_table_cache_updates_gen.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/modules.h"

namespace mongo {

using resharding::ParticipantShardsAndChunks;

/**
 * Represents the interface that ReshardingCoordinator uses to interact with the rest of the
 * sharding codebase.
 *
 * In particular, ReshardingCoordinator must not directly use Grid, ShardingState, or
 * ShardingCatalogClient. ReshardingCoordinator must instead access those types through the
 * ReshardingCoordinatorExternalState interface. Having it behind an interface makes it more
 * straightforward to unit test ReshardingCoordinator.
 */

class ReshardingCoordinatorExternalState {
public:
    virtual ~ReshardingCoordinatorExternalState() = default;

    virtual ParticipantShardsAndChunks calculateParticipantShardsAndChunks(
        OperationContext* opCtx,
        const ReshardingCoordinatorDocument& coordinatorDoc,
        std::vector<ReshardingZoneType> zones) = 0;

    virtual bool searchIndexExistsForCollection(OperationContext* opCtx,
                                                const NamespaceString& nss) = 0;

    ChunkVersion calculateChunkVersionForInitialChunks(OperationContext* opCtx);

    bool getIsUnsplittable(OperationContext* opCtx, const NamespaceString& nss);

    virtual void tellAllDonorsToRefresh(OperationContext* opCtx,
                                        const NamespaceString& sourceNss,
                                        const UUID& reshardingUUID,
                                        const std::vector<mongo::DonorShardEntry>& donorShards,
                                        const std::shared_ptr<executor::TaskExecutor>& executor,
                                        CancellationToken token) = 0;

    virtual void tellAllRecipientsToRefresh(
        OperationContext* opCtx,
        const NamespaceString& nssToRefresh,
        const UUID& reshardingUUID,
        const std::vector<mongo::RecipientShardEntry>& recipientShards,
        const std::shared_ptr<executor::TaskExecutor>& executor,
        CancellationToken token) = 0;

    virtual void establishAllDonorsAsParticipants(
        OperationContext* opCtx,
        const NamespaceString& sourceNss,
        const std::vector<mongo::DonorShardEntry>& donorShards,
        const std::shared_ptr<executor::TaskExecutor>& executor,
        CancellationToken token) = 0;

    virtual void establishAllRecipientsAsParticipants(
        OperationContext* opCtx,
        const NamespaceString& tempNss,
        const std::vector<mongo::RecipientShardEntry>& recipientShards,
        const std::shared_ptr<executor::TaskExecutor>& executor,
        CancellationToken token) = 0;

    /**
     * Returns a map from each donor shard id to the number of documents copied from that donor
     * shard by performing at snapshot read at the clone timestamp on that shard. 'shardVersions'
     * is map from the each donor shard id to the shard version of the collection on that donor
     * shard at the clone timestamp.
     */
    virtual std::map<ShardId, int64_t> getDocumentsToCopyFromDonors(
        OperationContext* opCtx,
        const std::shared_ptr<executor::TaskExecutor>& executor,
        CancellationToken token,
        const UUID& reshardingUUID,
        const NamespaceString& nss,
        const Timestamp& cloneTimestamp,
        const std::map<ShardId, ShardVersion>& shardVersions) = 0;

    /**
     * Returns a map from each donor shard id to the change in the number of documents in the
     * collection being resharded between the clone timestamp and blocking-writes timestamp on that
     * shard.
     */
    virtual std::map<ShardId, int64_t> getDocumentsDeltaFromDonors(
        OperationContext* opCtx,
        const std::shared_ptr<executor::TaskExecutor>& executor,
        CancellationToken token,
        const UUID& reshardingUUID,
        const NamespaceString& nss,
        const std::vector<ShardId>& shardIds) = 0;

    /**
     * Returns a map from each recipient shard id to the change in the number of documents in the
     * temporary resharding collection since the cloning phase started, as reported by the
     * recipient's change stream monitor.
     */
    virtual std::map<ShardId, int64_t> getDocumentsDeltaFromRecipients(
        OperationContext* opCtx,
        const std::shared_ptr<executor::TaskExecutor>& executor,
        CancellationToken token,
        const UUID& reshardingUUID,
        const NamespaceString& nss,
        const std::vector<ShardId>& shardIds) = 0;

    /**
     * To be called before transitioning to the "applying" state to verify the temporary collection
     * after cloning by asserting that:
     * - The total number of documents to copy is equal to the total number of documents copied.
     * - The number of documents to copy from each donor is equal to the total number of documents
     *   copied from that donor across all the recipients.
     */
    virtual void verifyClonedCollection(OperationContext* opCtx,
                                        const std::shared_ptr<executor::TaskExecutor>& executor,
                                        CancellationToken token,
                                        const ReshardingCoordinatorDocument& coordinatorDoc) = 0;

    /**
     * To be called before transitioning to the "committing" state to verify the temporary
     * collection after reaching strict consistency by asserting on the number of the documents.
     */
    virtual void verifyFinalCollection(OperationContext* opCtx,
                                       const ReshardingCoordinatorDocument& coordinatorDoc) = 0;

    /**
     * To be called during the "initializing" state to set allowMigrations to preventing new chunk
     * migrations from starting and aborting any in-progress migrations on the source collection.
     */
    virtual void stopMigrations(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& expectedCollectionUUID,
        ReshardingAuthoritativeMetadataAccessLevelEnum authoritativeMetadataLevel,
        std::function<OperationSessionInfo()> osiGenerator) = 0;

    /**
     * To be called on completion (both success and abort) to unset allowMigrations, re-enabling
     * chunk migrations on the source collection.
     */
    virtual void resumeMigrations(
        OperationContext* opCtx,
        const NamespaceString& nss,
        ReshardingAuthoritativeMetadataAccessLevelEnum authoritativeMetadataLevel,
        std::function<OperationSessionInfo()> osiGenerator) = 0;
    /**
     * Builds a CausalityBarrier for the given participant shards, which is used to perform a no-op
     * retryable write on each shard.
     */
    virtual std::unique_ptr<CausalityBarrier> buildCausalityBarrier(
        std::vector<ShardId> participants,
        std::shared_ptr<executor::TaskExecutor> executor,
        CancellationToken token) = 0;
};

class ReshardingCoordinatorExternalStateImpl final : public ReshardingCoordinatorExternalState {
public:
    ParticipantShardsAndChunks calculateParticipantShardsAndChunks(
        OperationContext* opCtx,
        const ReshardingCoordinatorDocument& coordinatorDoc,
        std::vector<ReshardingZoneType> zones) override;

    bool searchIndexExistsForCollection(OperationContext* opCtx,
                                        const NamespaceString& nss) override;

    void tellAllDonorsToRefresh(OperationContext* opCtx,
                                const NamespaceString& sourceNss,
                                const UUID& reshardingUUID,
                                const std::vector<mongo::DonorShardEntry>& donorShards,
                                const std::shared_ptr<executor::TaskExecutor>& executor,
                                CancellationToken token) override;

    void tellAllRecipientsToRefresh(OperationContext* opCtx,
                                    const NamespaceString& nssToRefresh,
                                    const UUID& reshardingUUID,
                                    const std::vector<mongo::RecipientShardEntry>& recipientShards,
                                    const std::shared_ptr<executor::TaskExecutor>& executor,
                                    CancellationToken token) override;

    void establishAllDonorsAsParticipants(OperationContext* opCtx,
                                          const NamespaceString& sourceNss,
                                          const std::vector<mongo::DonorShardEntry>& donorShards,
                                          const std::shared_ptr<executor::TaskExecutor>& executor,
                                          CancellationToken token) override;

    void establishAllRecipientsAsParticipants(
        OperationContext* opCtx,
        const NamespaceString& tempNss,
        const std::vector<mongo::RecipientShardEntry>& recipientShards,
        const std::shared_ptr<executor::TaskExecutor>& executor,
        CancellationToken token) override;

    std::map<ShardId, int64_t> getDocumentsToCopyFromDonors(
        OperationContext* opCtx,
        const std::shared_ptr<executor::TaskExecutor>& executor,
        CancellationToken token,
        const UUID& reshardingUUID,
        const NamespaceString& nss,
        const Timestamp& cloneTimestamp,
        const std::map<ShardId, ShardVersion>& shardVersions) override;

    std::map<ShardId, int64_t> getDocumentsDeltaFromDonors(
        OperationContext* opCtx,
        const std::shared_ptr<executor::TaskExecutor>& executor,
        CancellationToken token,
        const UUID& reshardingUUID,
        const NamespaceString& nss,
        const std::vector<ShardId>& shardIds) override;

    std::map<ShardId, int64_t> getDocumentsDeltaFromRecipients(
        OperationContext* opCtx,
        const std::shared_ptr<executor::TaskExecutor>& executor,
        CancellationToken token,
        const UUID& reshardingUUID,
        const NamespaceString& nss,
        const std::vector<ShardId>& shardIds) override;

    void verifyClonedCollection(OperationContext* opCtx,
                                const std::shared_ptr<executor::TaskExecutor>& executor,
                                CancellationToken token,
                                const ReshardingCoordinatorDocument& coordinatorDoc) override;

    void verifyFinalCollection(OperationContext* opCtx,
                               const ReshardingCoordinatorDocument& coordinatorDoc) override;

    void stopMigrations(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const UUID& expectedCollectionUUID,
                        ReshardingAuthoritativeMetadataAccessLevelEnum authoritativeMetadataLevel,
                        std::function<OperationSessionInfo()> osiGenerator) override;

    void resumeMigrations(OperationContext* opCtx,
                          const NamespaceString& nss,
                          ReshardingAuthoritativeMetadataAccessLevelEnum authoritativeMetadataLevel,
                          std::function<OperationSessionInfo()> osiGenerator) override;

    std::unique_ptr<CausalityBarrier> buildCausalityBarrier(
        std::vector<ShardId> participants,
        std::shared_ptr<executor::TaskExecutor> executor,
        CancellationToken token) override;

private:
    /**
     * Returns a map from each donor shard id to the number of documents copied from that donor
     * shard based on the metrics in the recipient collection cloner resume data documents.
     */
    std::map<ShardId, int64_t> _getDocumentsCopiedFromRecipients(
        OperationContext* opCtx,
        const std::shared_ptr<executor::TaskExecutor>& executor,
        CancellationToken token,
        const UUID& reshardingUUID,
        const std::vector<ShardId>& shardIds);
};

}  // namespace mongo
