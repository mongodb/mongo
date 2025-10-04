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

#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/local_catalog/shard_role_catalog/flush_routing_table_cache_updates_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_dao.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/uuid.h"

#include <vector>

namespace mongo {

class ReshardingMetrics;
class ReshardingCoordinatorDocument;

namespace resharding {

typedef unique_function<ReshardingCoordinatorDocument(OperationContext*, TxnNumber)>
    PhaseTransitionFn;

CollectionType createTempReshardingCollectionType(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    const ChunkVersion& chunkVersion,
    const BSONObj& collation,
    boost::optional<bool> isUnsplittable);

void removeChunkDocs(OperationContext* opCtx, const UUID& collUUID);

void writeDecisionPersistedState(OperationContext* opCtx,
                                 ReshardingMetrics* metrics,
                                 const ReshardingCoordinatorDocument& coordinatorDoc,
                                 OID newCollectionEpoch,
                                 Timestamp newCollectionTimestamp,
                                 const std::vector<ShardId>& reshardedCollectionPlacement);

void updateTagsDocsForTempNss(OperationContext* opCtx,
                              const ReshardingCoordinatorDocument& coordinatorDoc,
                              TxnNumber txnNumber);

void insertCoordDocAndChangeOrigCollEntry(OperationContext* opCtx,
                                          ReshardingMetrics* metrics,
                                          const ReshardingCoordinatorDocument& coordinatorDoc);

void writeParticipantShardsAndTempCollInfo(OperationContext* opCtx,
                                           ReshardingMetrics* metrics,
                                           const ReshardingCoordinatorDocument& coordinatorDoc,
                                           PhaseTransitionFn phaseTransitionFn,
                                           std::vector<ChunkType> initialChunks,
                                           std::vector<ReshardingZoneType> zones,
                                           boost::optional<bool> isUnsplittable);

void writeStateTransitionAndCatalogUpdatesThenBumpCollectionPlacementVersions(
    OperationContext* opCtx,
    ReshardingMetrics* metrics,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    boost::optional<PhaseTransitionFn> phaseTransitionFn = boost::none);

ReshardingCoordinatorDocument removeOrQuiesceCoordinatorDocAndRemoveReshardingFields(
    OperationContext* opCtx,
    ReshardingMetrics* metrics,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    boost::optional<Status> abortReason = boost::none);

void writeToCoordinatorStateNss(OperationContext* opCtx,
                                ReshardingMetrics* metrics,
                                const ReshardingCoordinatorDocument& coordinatorDoc,
                                TxnNumber txnNumber);

void executeMetadataChangesInTxn(
    OperationContext* opCtx,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc);

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

void sendFlushReshardingStateChangeToShards(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const UUID& reshardingUUID,
                                            const std::vector<ShardId>& shardIds,
                                            const std::shared_ptr<executor::TaskExecutor>& executor,
                                            CancellationToken token);

void sendFlushRoutingTableCacheUpdatesToShards(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const std::vector<ShardId>& shardIds,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    CancellationToken token);

TypeCollectionRecipientFields constructRecipientFields(
    const ReshardingCoordinatorDocument& coordinatorDoc);

struct ShardOwnership {
    std::set<ShardId> shardsOwningChunks;
    std::set<ShardId> shardsNotOwningChunks;
};

ShardOwnership computeRecipientChunkOwnership(OperationContext* opCtx,
                                              const ReshardingCoordinatorDocument& coordinatorDoc);

void assertResultIsValidForUpdatesAndDeletes(const BatchedCommandRequest& request,
                                             const BSONObj& result);

void writeToConfigCollectionsForTempNss(OperationContext* opCtx,
                                        const ReshardingCoordinatorDocument& coordinatorDoc,
                                        boost::optional<ChunkVersion> chunkVersion,
                                        boost::optional<const BSONObj&> collation,
                                        boost::optional<bool> isUnsplittable,
                                        TxnNumber txnNumber);

BatchedCommandRequest generateBatchedCommandRequestForConfigCollectionsForTempNss(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    boost::optional<ChunkVersion> chunkVersion,
    boost::optional<const BSONObj&> collation,
    boost::optional<bool> isUnsplittable);
}  // namespace resharding

}  // namespace mongo
