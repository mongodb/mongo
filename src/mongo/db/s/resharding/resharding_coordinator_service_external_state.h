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

#include "mongo/db/operation_context.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/request_types/flush_routing_table_cache_updates_gen.h"
#include "mongo/s/resharding/common_types_gen.h"

namespace mongo {

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
    struct ParticipantShardsAndChunks {
        std::vector<DonorShardEntry> donorShards;
        std::vector<RecipientShardEntry> recipientShards;
        std::vector<ChunkType> initialChunks;
    };

    virtual ~ReshardingCoordinatorExternalState() = default;

    virtual ParticipantShardsAndChunks calculateParticipantShardsAndChunks(
        OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) = 0;

    ChunkVersion calculateChunkVersionForInitialChunks(OperationContext* opCtx);

    bool getIsUnsplittable(OperationContext* opCtx, const NamespaceString& nss);

    template <typename CommandType>
    std::vector<AsyncRequestsSender::Response> sendCommandToShards(
        OperationContext* opCtx,
        std::shared_ptr<async_rpc::AsyncRPCOptions<CommandType>> opts,
        const std::vector<ShardId>& shardIds) {
        return sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, shardIds);
    }

    template <typename CommandType>
    std::vector<AsyncRequestsSender::Response> sendCommandToShards(
        OperationContext* opCtx,
        std::shared_ptr<async_rpc::AsyncRPCOptions<CommandType>> opts,
        const std::map<ShardId, ShardVersion>& shardVersions,
        const ReadPreferenceSetting& readPref) {
        return sharding_ddl_util::sendAuthenticatedCommandToShards(
            opCtx, opts, shardVersions, readPref, true /* throwOnError */);
    }

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
};

class ReshardingCoordinatorExternalStateImpl final : public ReshardingCoordinatorExternalState {
public:
    ParticipantShardsAndChunks calculateParticipantShardsAndChunks(
        OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) override;

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

    void verifyClonedCollection(OperationContext* opCtx,
                                const std::shared_ptr<executor::TaskExecutor>& executor,
                                CancellationToken token,
                                const ReshardingCoordinatorDocument& coordinatorDoc) override;

    void verifyFinalCollection(OperationContext* opCtx,
                               const ReshardingCoordinatorDocument& coordinatorDoc) override;

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
