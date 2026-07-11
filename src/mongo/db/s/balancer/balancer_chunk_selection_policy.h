// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/db/s/balancer/cluster_statistics.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <unordered_set>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace balancer_policy_utils {
/*
 * Helper to check if a collection is explicitly disabled for balancing
 */
bool canBalanceCollection(const CollectionType& coll);

}  // namespace balancer_policy_utils
/**
 * Class used by the balancer for selecting chunks, which need to be moved around in order for
 * the sharded cluster to be balanced.
 */
class BalancerChunkSelectionPolicy {
    BalancerChunkSelectionPolicy(const BalancerChunkSelectionPolicy&) = delete;
    BalancerChunkSelectionPolicy& operator=(const BalancerChunkSelectionPolicy&) = delete;

public:
    explicit BalancerChunkSelectionPolicy(ClusterStatistics* clusterStats);

    /**
     * Potentially blocking method, which gives out a set of chunks, which need to be split because
     * they violate the policy for some reason. The reason is decided by the policy and may include
     * chunk is too big or chunk straddles a zone range.
     */
    StatusWith<SplitInfoVector> selectChunksToSplit(OperationContext* opCtx);

    /**
     * Given a valid namespace returns all the splits the balancer would need to perform with the
     * current state
     */
    StatusWith<SplitInfoVector> selectChunksToSplit(OperationContext* opCtx,
                                                    const NamespaceString& ns);

    /**
     * Potentially blocking method, which gives out a set of chunks to be moved.
     */
    StatusWith<MigrateInfoVector> selectChunksToMove(
        OperationContext* opCtx,
        const std::vector<ClusterStatistics::ShardStatistics>& shardStats,
        stdx::unordered_set<ShardId>* availableShards,
        stdx::unordered_set<NamespaceString>* imbalancedCollectionsCachePtr);

    /**
     * Given a valid namespace returns all the Migrations the balancer would need to perform with
     * the current state.
     */
    StatusWith<MigrateInfosWithReason> selectChunksToMove(OperationContext* opCtx,
                                                          const NamespaceString& ns);

private:
    /**
     * Synchronous method, which iterates the collection's chunks and uses the zones information to
     * figure out whether some of them validate the zone range boundaries and need to be split.
     */
    StatusWith<SplitInfoVector> _getSplitCandidatesForCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ShardStatisticsVector& shardStats);

    /**
     * Synchronous method, which iterates the collection's size per shard  to figure out where to
     * place them.
     */
    StatusWith<MigrateInfosWithReason> _getMigrateCandidatesForCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ShardStatisticsVector& shardStats,
        const CollectionDataSizeInfoForBalancing& collDataSizeInfo,
        stdx::unordered_set<ShardId>* availableShards);

    // Source for obtaining cluster statistics. Not owned and must not be destroyed before the
    // policy object is destroyed.
    ClusterStatistics* const _clusterStats;
};

}  // namespace mongo
