/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/s/balancer/balancer_chunk_selection_policy.h"

namespace mongo {

class ClusterStatistics;

class BalancerChunkSelectionPolicyImpl final : public BalancerChunkSelectionPolicy {
public:
    BalancerChunkSelectionPolicyImpl(std::unique_ptr<ClusterStatistics> clusterStats);
    ~BalancerChunkSelectionPolicyImpl();

    StatusWith<SplitInfoVector> selectChunksToSplit(OperationContext* txn) override;

    StatusWith<MigrateInfoVector> selectChunksToMove(OperationContext* txn,
                                                     bool aggressiveBalanceHint) override;

    StatusWith<boost::optional<MigrateInfo>> selectSpecificChunkToMove(
        OperationContext* txn, const ChunkType& chunk) override;

    Status checkMoveAllowed(OperationContext* txn,
                            const ChunkType& chunk,
                            const ShardId& newShardId) override;

private:
    /**
     * Synchronous method, which iterates the collection's chunks and uses the tags information to
     * figure out whether some of them validate the tag range boundaries and need to be split.
     */
    StatusWith<SplitInfoVector> _getSplitCandidatesForCollection(
        OperationContext* txn, const NamespaceString& nss, const ShardStatisticsVector& shardStats);

    /**
     * Synchronous method, which iterates the collection's chunks and uses the cluster statistics to
     * figure out where to place them.
     */
    StatusWith<MigrateInfoVector> _getMigrateCandidatesForCollection(
        OperationContext* txn,
        const NamespaceString& nss,
        const ShardStatisticsVector& shardStats,
        bool aggressiveBalanceHint);

    // Source for obtaining cluster statistics
    std::unique_ptr<ClusterStatistics> _clusterStats;
};

}  // namespace mongo
