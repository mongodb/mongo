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

    boost::optional<CollectionIndexes> getCatalogIndexVersion(OperationContext* opCtx,
                                                              const NamespaceString& nss,
                                                              const UUID& uuid);

    bool getIsUnsplittable(OperationContext* opCtx, const NamespaceString& nss);

    boost::optional<CollectionIndexes> getCatalogIndexVersionForCommit(OperationContext* opCtx,
                                                                       const NamespaceString& nss);

    template <typename CommandType>
    void sendCommandToShards(OperationContext* opCtx,
                             std::shared_ptr<async_rpc::AsyncRPCOptions<CommandType>> opts,
                             const std::vector<ShardId>& shardIds) {
        sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, shardIds);
    }
};

class ReshardingCoordinatorExternalStateImpl final : public ReshardingCoordinatorExternalState {
public:
    ParticipantShardsAndChunks calculateParticipantShardsAndChunks(
        OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) override;
};

}  // namespace mongo
