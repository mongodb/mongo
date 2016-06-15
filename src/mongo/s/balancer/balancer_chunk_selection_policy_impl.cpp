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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/balancer/balancer_chunk_selection_policy_impl.h"

#include <set>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_raii.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using ChunkMinimumsSet = std::set<BSONObj>;
using MigrateInfoVector = BalancerChunkSelectionPolicy::MigrateInfoVector;
using SplitInfoVector = BalancerChunkSelectionPolicy::SplitInfoVector;
using std::shared_ptr;
using std::unique_ptr;
using std::vector;

namespace {

/**
 * Does a linear pass over the information cached in the specified chunk manager and extracts chunk
 * distrubution and chunk placement information which is needed by the balancer policy.
 */
std::pair<ShardToChunksMap, ChunkMinimumsSet> createCollectionDistributionInfo(
    const ShardStatisticsVector& allShards, ChunkManager* chunkMgr) {
    ShardToChunksMap shardToChunksMap;
    ChunkMinimumsSet chunkMinimums;

    // Makes sure there is an entry in shardToChunksMap for every shard, so empty shards will also
    // be accounted for
    for (const auto& stat : allShards) {
        shardToChunksMap[stat.shardId];
    }

    for (const auto& entry : chunkMgr->getChunkMap()) {
        const auto& chunkEntry = entry.second;

        ChunkType chunk;
        chunk.setMin(chunkEntry->getMin());
        chunk.setMax(chunkEntry->getMax());
        chunk.setJumbo(chunkEntry->isJumbo());
        chunk.setShard(chunkEntry->getShardId());

        shardToChunksMap[chunkEntry->getShardId()].push_back(chunk);
        chunkMinimums.insert(chunkEntry->getMin());
    }

    return std::make_pair(std::move(shardToChunksMap), std::move(chunkMinimums));
}

}  // namespace

BalancerChunkSelectionPolicyImpl::BalancerChunkSelectionPolicyImpl(
    std::unique_ptr<ClusterStatistics> clusterStats)
    : _clusterStats(std::move(clusterStats)) {}

BalancerChunkSelectionPolicyImpl::~BalancerChunkSelectionPolicyImpl() = default;

StatusWith<SplitInfoVector> BalancerChunkSelectionPolicyImpl::selectChunksToSplit(
    OperationContext* txn) {
    vector<CollectionType> collections;

    Status collsStatus =
        Grid::get(txn)->catalogClient(txn)->getCollections(txn, nullptr, &collections, nullptr);
    if (!collsStatus.isOK()) {
        return collsStatus;
    }

    if (collections.empty()) {
        return SplitInfoVector{};
    }

    SplitInfoVector splitCandidates;

    for (const auto& coll : collections) {
        const NamespaceString nss(coll.getNs());

        auto candidatesStatus = _getSplitCandidatesForCollection(txn, nss);
        if (!candidatesStatus.isOK()) {
            warning() << "Unable to enforce tag range policy for collection " << nss.ns()
                      << causedBy(candidatesStatus.getStatus());
            continue;
        }

        splitCandidates.insert(splitCandidates.end(),
                               std::make_move_iterator(candidatesStatus.getValue().begin()),
                               std::make_move_iterator(candidatesStatus.getValue().end()));
    }

    return splitCandidates;
}

StatusWith<MigrateInfoVector> BalancerChunkSelectionPolicyImpl::selectChunksToMove(
    OperationContext* txn, bool aggressiveBalanceHint) {
    auto shardStatsStatus = _clusterStats->getStats(txn);
    if (!shardStatsStatus.isOK()) {
        return shardStatsStatus.getStatus();
    }

    const auto shardStats = std::move(shardStatsStatus.getValue());
    if (shardStats.size() < 2) {
        return MigrateInfoVector{};
    }

    vector<CollectionType> collections;

    Status collsStatus =
        Grid::get(txn)->catalogClient(txn)->getCollections(txn, nullptr, &collections, nullptr);
    if (!collsStatus.isOK()) {
        return collsStatus;
    }

    if (collections.empty()) {
        return MigrateInfoVector{};
    }

    MigrateInfoVector candidateChunks;

    for (const auto& coll : collections) {
        const NamespaceString nss(coll.getNs());

        if (!coll.getAllowBalance()) {
            LOG(1) << "Not balancing collection " << nss << "; explicitly disabled.";
            continue;
        }

        auto candidatesStatus =
            _getMigrateCandidatesForCollection(txn, nss, shardStats, aggressiveBalanceHint);
        if (!candidatesStatus.isOK()) {
            warning() << "Unable to balance collection " << nss.ns()
                      << causedBy(candidatesStatus.getStatus());
            continue;
        }

        candidateChunks.insert(candidateChunks.end(),
                               std::make_move_iterator(candidatesStatus.getValue().begin()),
                               std::make_move_iterator(candidatesStatus.getValue().end()));
    }

    return candidateChunks;
}

StatusWith<boost::optional<MigrateInfo>>
BalancerChunkSelectionPolicyImpl::selectSpecificChunkToMove(OperationContext* txn,
                                                            const ChunkType& chunk) {
    const NamespaceString nss(chunk.getNS());

    auto scopedCMStatus = ScopedChunkManager::getExisting(txn, nss);
    if (!scopedCMStatus.isOK()) {
        return scopedCMStatus.getStatus();
    }

    auto scopedCM = std::move(scopedCMStatus.getValue());
    ChunkManager* const cm = scopedCM.cm();

    auto tagForChunkStatus =
        Grid::get(txn)->catalogClient(txn)->getTagForChunk(txn, nss.ns(), chunk);
    if (!tagForChunkStatus.isOK()) {
        return tagForChunkStatus.getStatus();
    }

    auto shardStatsStatus = _clusterStats->getStats(txn);
    if (!shardStatsStatus.isOK()) {
        return shardStatsStatus.getStatus();
    }

    auto collInfo = createCollectionDistributionInfo(shardStatsStatus.getValue(), cm);
    ShardToChunksMap shardToChunksMap = std::move(std::get<0>(collInfo));

    DistributionStatus distStatus(shardStatsStatus.getValue(), shardToChunksMap);
    const ShardId newShardId(distStatus.getBestReceieverShard(tagForChunkStatus.getValue()));
    if (!newShardId.isValid() || newShardId == chunk.getShard()) {
        return boost::optional<MigrateInfo>();
    }

    return boost::optional<MigrateInfo>{MigrateInfo(nss.ns(), newShardId, chunk)};
}

Status BalancerChunkSelectionPolicyImpl::checkMoveAllowed(OperationContext* txn,
                                                          const ChunkType& chunk,
                                                          const ShardId& newShardId) {
    auto tagForChunkStatus =
        Grid::get(txn)->catalogClient(txn)->getTagForChunk(txn, chunk.getNS(), chunk);
    if (!tagForChunkStatus.isOK()) {
        return tagForChunkStatus.getStatus();
    }

    auto shardStatsStatus = _clusterStats->getStats(txn);
    if (!shardStatsStatus.isOK()) {
        return shardStatsStatus.getStatus();
    }

    const auto& shardStats = shardStatsStatus.getValue();

    auto newShardIterator =
        std::find_if(shardStats.begin(),
                     shardStats.end(),
                     [&newShardId](const ClusterStatistics::ShardStatistics& stat) {
                         return stat.shardId == newShardId;
                     });
    if (newShardIterator == shardStats.end()) {
        return {ErrorCodes::ShardNotFound,
                str::stream() << "Unable to find constraints information for shard " << newShardId
                              << ". Move to this shard will be disallowed."};
    }

    return DistributionStatus::isShardSuitableReceiver(*newShardIterator,
                                                       tagForChunkStatus.getValue());
}

StatusWith<SplitInfoVector> BalancerChunkSelectionPolicyImpl::_getSplitCandidatesForCollection(
    OperationContext* txn, const NamespaceString& nss) {
    auto scopedCMStatus = ScopedChunkManager::getExisting(txn, nss);
    if (!scopedCMStatus.isOK()) {
        return scopedCMStatus.getStatus();
    }

    auto scopedCM = std::move(scopedCMStatus.getValue());
    ChunkManager* const cm = scopedCM.cm();

    vector<TagsType> collectionTags;
    Status tagsStatus =
        Grid::get(txn)->catalogClient(txn)->getTagsForCollection(txn, nss.ns(), &collectionTags);
    if (!tagsStatus.isOK()) {
        return {tagsStatus.code(),
                str::stream() << "Unable to load tags for collection " << nss.ns() << " due to "
                              << tagsStatus.toString()};
    }

    auto collInfo = createCollectionDistributionInfo({}, cm);
    ChunkMinimumsSet allChunkMinimums = std::move(std::get<1>(collInfo));

    SplitInfoVector splitCandidates;

    for (const auto& tagInfo : collectionTags) {
        BSONObj min =
            cm->getShardKeyPattern().getKeyPattern().extendRangeBound(tagInfo.getMinKey(), false);

        if (allChunkMinimums.count(min)) {
            continue;
        }

        shared_ptr<Chunk> chunk = cm->findIntersectingChunk(txn, min);
        invariant(chunk);

        splitCandidates.emplace_back(
            chunk->getShardId(), nss, cm->getVersion(), chunk->getMin(), chunk->getMax(), min);
    }

    return splitCandidates;
}

StatusWith<MigrateInfoVector> BalancerChunkSelectionPolicyImpl::_getMigrateCandidatesForCollection(
    OperationContext* txn,
    const NamespaceString& nss,
    const ShardStatisticsVector& shardStats,
    bool aggressiveBalanceHint) {
    auto scopedCMStatus = ScopedChunkManager::getExisting(txn, nss);
    if (!scopedCMStatus.isOK()) {
        return scopedCMStatus.getStatus();
    }

    auto scopedCM = std::move(scopedCMStatus.getValue());
    ChunkManager* const cm = scopedCM.cm();

    auto collInfo = createCollectionDistributionInfo(shardStats, cm);
    ShardToChunksMap shardToChunksMap = std::move(std::get<0>(collInfo));
    ChunkMinimumsSet allChunkMinimums = std::move(std::get<1>(collInfo));

    DistributionStatus distStatus(shardStats, shardToChunksMap);
    {
        vector<TagsType> collectionTags;
        Status status = Grid::get(txn)->catalogClient(txn)->getTagsForCollection(
            txn, nss.ns(), &collectionTags);
        if (!status.isOK()) {
            return status;
        }

        for (const auto& tagInfo : collectionTags) {
            BSONObj min = cm->getShardKeyPattern().getKeyPattern().extendRangeBound(
                tagInfo.getMinKey(), false);

            if (!allChunkMinimums.count(min)) {
                // This tag falls somewhere at the middle of a chunk. Therefore we must skip
                // balancing this collection until it is split at the next iteration.
                //
                // TODO: We should be able to just skip chunks, which straddle tags and still make
                // some progress balancing.
                return {ErrorCodes::IllegalOperation,
                        str::stream()
                            << "Tag boundaries "
                            << tagInfo.toString()
                            << " fall in the middle of an existing chunk. Balancing for collection "
                            << nss.ns()
                            << " will be postponed until the chunk is split appropriately."};
            }

            // TODO: TagRange contains all the information from TagsType except for the namespace,
            // so maybe the two can be merged at some point in order to avoid the transformation
            // below.
            if (!distStatus.addTagRange(TagRange(tagInfo.getMinKey().getOwned(),
                                                 tagInfo.getMaxKey().getOwned(),
                                                 tagInfo.getTag()))) {
                return {ErrorCodes::BadValue,
                        str::stream() << "Tag ranges are not valid for collection " << nss.ns()
                                      << ". Balancing for this collection will be skipped until "
                                         "the ranges are fixed."};
            }
        }
    }

    unique_ptr<MigrateInfo> migrateInfo(
        BalancerPolicy::balance(nss.ns(), distStatus, aggressiveBalanceHint));
    if (migrateInfo) {
        return MigrateInfoVector{*migrateInfo};
    }

    return MigrateInfoVector{};
}

}  // namespace mongo
