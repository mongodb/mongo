/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/s/balancer/balancer_chunk_selection_policy.h"
#include "mongo/db/s/balancer/balancer_random.h"

namespace mongo {

class ClusterStatistics;

class BalancerChunkSelectionPolicyImpl final : public BalancerChunkSelectionPolicy {
public:
    BalancerChunkSelectionPolicyImpl(ClusterStatistics* clusterStats, BalancerRandomSource& random);
    ~BalancerChunkSelectionPolicyImpl();

    StatusWith<SplitInfoVector> selectChunksToSplit(OperationContext* opCtx) override;

    StatusWith<SplitInfoVector> selectChunksToSplit(OperationContext* opCtx,
                                                    const NamespaceString& ns) override;

    StatusWith<MigrateInfoVector> selectChunksToMove(
        OperationContext* opCtx, stdx::unordered_set<ShardId>* usedShards) override;

    StatusWith<MigrateInfosWithReason> selectChunksToMove(OperationContext* opCtx,
                                                          const NamespaceString& ns) override;

    StatusWith<boost::optional<MigrateInfo>> selectSpecificChunkToMove(
        OperationContext* opCtx, const NamespaceString& nss, const ChunkType& chunk) override;

    Status checkMoveAllowed(OperationContext* opCtx,
                            const ChunkType& chunk,
                            const ShardId& newShardId) override;

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
     * Synchronous method, which iterates the collection's chunks and uses the cluster statistics to
     * figure out where to place them.
     */
    StatusWith<MigrateInfosWithReason> _getMigrateCandidatesForCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ShardStatisticsVector& shardStats,
        const boost::optional<CollectionDataSizeInfoForBalancing>& collDataSizeInfo,
        stdx::unordered_set<ShardId>* usedShards);

    // Source for obtaining cluster statistics. Not owned and must not be destroyed before the
    // policy object is destroyed.
    ClusterStatistics* const _clusterStats;

    // Source of randomness when metadata needs to be randomized.
    BalancerRandomSource& _random;
};

}  // namespace mongo
