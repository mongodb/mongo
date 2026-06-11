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
#include "mongo/db/s/resharding/resharding_coordinator_dao.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/shard_catalog/flush_routing_table_cache_updates_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/modules.h"
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
                                 const std::vector<ShardRef>& reshardedCollectionPlacement);

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

/**
 * Builds the structurally-required lifecycle write to config.collections for the temporary
 * resharding namespace: an insert at kPreparingToDonate (which materializes the temp entry)
 * and a delete at kCommitting (which removes it).
 *
 * Returns boost::none for any other state.
 */
boost::optional<BatchedCommandRequest> createTempCollectionLifecycleRequest(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    boost::optional<ChunkVersion> chunkVersion,
    boost::optional<const BSONObj&> collation,
    boost::optional<bool> isUnsplittable);

/**
 * Builds the legacy 'reshardingFields' partial-update write to the temp-nss config.collections
 * entry for transient states (kCloning and the catch-all default branch covering kApplying,
 * kBlockingWrites, kAborting, kQuiesced, and kDone). Callers must first consult
 * 'skipReshardingFieldsWritesForCoordinator' to decide whether the legacy path applies.
 *
 * Returns boost::none for kPreparingToDonate and kCommitting, which are handled by
 * 'createTempCollectionLifecycleRequest'.
 */
boost::optional<BatchedCommandRequest> createLegacyTempCollectionReshardingFieldsRequest(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc);

/**
 * Returns true when the coordinator should skip writing the 'reshardingFields' subtree to
 * config.collections. Under 'featureFlagReshardingInitNoRefresh' participants are initialized
 * via explicit shardsvr commands instead of the refresh-driven path, so the subtree is unused;
 * writing it via partial $set would produce a parent missing IDL-required fields.
 *
 * Uses the VersionContext pinned on the coordinator doc so the decision is stable across FCV
 * transitions for the operation's lifetime. Callers should consult this predicate before
 * invoking 'createLegacyReshardingFieldsUpdate' for the original nss, or before populating
 * 'reshardingFields' on a temp-nss CollectionType via 'createTempReshardingCollectionType'.
 */
bool skipReshardingFieldsWritesForCoordinator(const ReshardingCoordinatorDocument& coordinatorDoc);

/**
 * Builds the legacy 'reshardingFields' update for the original-nss config.collections entry,
 * shaped per the coordinator's current state. The function does not gate itself; callers are
 * expected to first check 'skipReshardingFieldsWritesForCoordinator' to decide whether the
 * legacy path applies.
 */
BSONObj createLegacyReshardingFieldsUpdate(OperationContext* opCtx,
                                           const ReshardingCoordinatorDocument& coordinatorDoc);

/**
 * Builds the collection-identity swap for the original-nss config.collections entry during
 * kCommitting: sets 'uuid', 'key', 'lastmodEpoch', 'lastmod', and (when provided) 'timestamp', plus
 * 'unsplittable: true' for unshardCollection provenance. This runs unconditionally at commit and
 * is what flips config.collections.<sourceNss> from the pre-resharding identity to the resharded
 * one.
 */
BSONObj createReshardedCollectionEntryUpdate(OperationContext* opCtx,
                                             const ReshardingCoordinatorDocument& coordinatorDoc,
                                             OID newCollectionEpoch,
                                             boost::optional<Timestamp> newCollectionTimestamp);

boost::optional<UUID> tryRetrieveReshardingUUID(OperationContext* opCtx, const NamespaceString& ns);

UUID retrieveReshardingUUID(OperationContext* opCtx, const NamespaceString& ns);

/**
 * Returns the deadline the coordinator uses to bound how long it waits for the donor and recipient
 * deltas during pre-commit verification before giving up and proceeding to commit.
 * 'reachedStrictConsistencyTime' is the time all recipients reached strict consistency; the
 * deadline is a configured share of the critical-section time still remaining at that point.
 */
Date_t computeVerificationDeadline(const ReshardingCoordinatorDocument& coordinatorDoc,
                                   Date_t reachedStrictConsistencyTime);
}  // namespace resharding

}  // namespace mongo
