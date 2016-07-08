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
StatusWith<std::pair<DistributionStatus, ChunkMinimumsSet>> createCollectionDistributionInfo(
    OperationContext* txn, const ShardStatisticsVector& allShards, ChunkManager* chunkMgr) {
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

    vector<TagsType> collectionTags;
    Status tagsStatus = Grid::get(txn)->catalogClient(txn)->getTagsForCollection(
        txn, chunkMgr->getns(), &collectionTags);
    if (!tagsStatus.isOK()) {
        return {tagsStatus.code(),
                str::stream() << "Unable to load tags for collection " << chunkMgr->getns()
                              << " due to "
                              << tagsStatus.toString()};
    }

    DistributionStatus distribution(NamespaceString(chunkMgr->getns()),
                                    std::move(shardToChunksMap));

    // Cache the collection tags
    const auto& keyPattern = chunkMgr->getShardKeyPattern().getKeyPattern();

    for (const auto& tag : collectionTags) {
        if (!distribution.addTagRange(TagRange(keyPattern.extendRangeBound(tag.getMinKey(), false),
                                               keyPattern.extendRangeBound(tag.getMaxKey(), false),
                                               tag.getTag()))) {
            return {ErrorCodes::BadValue,
                    str::stream() << "Tag ranges are not valid for collection " << chunkMgr->getns()
                                  << ". Balancing for this collection will be skipped until "
                                     "the ranges are fixed."};
        }
    }

    return std::make_pair(std::move(distribution), std::move(chunkMinimums));
}

}  // namespace

BalancerChunkSelectionPolicyImpl::BalancerChunkSelectionPolicyImpl(
    std::unique_ptr<ClusterStatistics> clusterStats)
    : _clusterStats(std::move(clusterStats)) {}

BalancerChunkSelectionPolicyImpl::~BalancerChunkSelectionPolicyImpl() = default;

StatusWith<SplitInfoVector> BalancerChunkSelectionPolicyImpl::selectChunksToSplit(
    OperationContext* txn) {
    auto shardStatsStatus = _clusterStats->getStats(txn);
    if (!shardStatsStatus.isOK()) {
        return shardStatsStatus.getStatus();
    }

    const auto shardStats = std::move(shardStatsStatus.getValue());

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

        auto candidatesStatus = _getSplitCandidatesForCollection(txn, nss, shardStats);
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
    auto shardStatsStatus = _clusterStats->getStats(txn);
    if (!shardStatsStatus.isOK()) {
        return shardStatsStatus.getStatus();
    }

    const auto shardStats = std::move(shardStatsStatus.getValue());

    const NamespaceString nss(chunk.getNS());

    auto scopedCMStatus = ScopedChunkManager::getExisting(txn, nss);
    if (!scopedCMStatus.isOK()) {
        return scopedCMStatus.getStatus();
    }

    auto scopedCM = std::move(scopedCMStatus.getValue());
    ChunkManager* const cm = scopedCM.cm();

    auto collInfoStatus = createCollectionDistributionInfo(txn, shardStats, cm);
    if (!collInfoStatus.isOK()) {
        return collInfoStatus.getStatus();
    }

    auto collInfo = std::move(collInfoStatus.getValue());

    return BalancerPolicy::balanceSingleChunk(chunk, shardStats, std::get<0>(collInfo));
}

Status BalancerChunkSelectionPolicyImpl::checkMoveAllowed(OperationContext* txn,
                                                          const ChunkType& chunk,
                                                          const ShardId& newShardId) {
    auto shardStatsStatus = _clusterStats->getStats(txn);
    if (!shardStatsStatus.isOK()) {
        return shardStatsStatus.getStatus();
    }

    auto shardStats = std::move(shardStatsStatus.getValue());

    const NamespaceString nss(chunk.getNS());

    auto scopedCMStatus = ScopedChunkManager::getExisting(txn, nss);
    if (!scopedCMStatus.isOK()) {
        return scopedCMStatus.getStatus();
    }

    auto scopedCM = std::move(scopedCMStatus.getValue());
    ChunkManager* const cm = scopedCM.cm();

    auto collInfoStatus = createCollectionDistributionInfo(txn, shardStats, cm);
    if (!collInfoStatus.isOK()) {
        return collInfoStatus.getStatus();
    }

    auto collInfo = std::move(collInfoStatus.getValue());

    DistributionStatus distribution = std::move(std::get<0>(collInfo));

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

    return BalancerPolicy::isShardSuitableReceiver(*newShardIterator,
                                                   distribution.getTagForChunk(chunk));
}

StatusWith<SplitInfoVector> BalancerChunkSelectionPolicyImpl::_getSplitCandidatesForCollection(
    OperationContext* txn, const NamespaceString& nss, const ShardStatisticsVector& shardStats) {
    auto scopedCMStatus = ScopedChunkManager::getExisting(txn, nss);
    if (!scopedCMStatus.isOK()) {
        return scopedCMStatus.getStatus();
    }

    auto scopedCM = std::move(scopedCMStatus.getValue());
    ChunkManager* const cm = scopedCM.cm();

    auto collInfoStatus = createCollectionDistributionInfo(txn, shardStats, cm);
    if (!collInfoStatus.isOK()) {
        return collInfoStatus.getStatus();
    }

    auto collInfo = std::move(collInfoStatus.getValue());

    DistributionStatus distribution = std::move(std::get<0>(collInfo));
    ChunkMinimumsSet allChunkMinimums = std::move(std::get<1>(collInfo));

    SplitInfoVector splitCandidates;

    // Accumulate split points for the same chunk together
    shared_ptr<Chunk> currentChunk;
    vector<BSONObj> currentSplitVector;

    for (const auto& tagRangeEntry : distribution.tagRanges()) {
        const auto& tagRange = tagRangeEntry.second;

        if (allChunkMinimums.count(tagRange.min)) {
            continue;
        }

        shared_ptr<Chunk> chunk = cm->findIntersectingChunk(txn, tagRange.min);

        if (!currentChunk) {
            currentChunk = chunk;
        }

        invariant(currentChunk);

        if (chunk == currentChunk) {
            currentSplitVector.push_back(tagRange.min);
        } else {
            splitCandidates.emplace_back(currentChunk->getShardId(),
                                         nss,
                                         cm->getVersion(),
                                         currentChunk->getLastmod(),
                                         currentChunk->getMin(),
                                         currentChunk->getMax(),
                                         std::move(currentSplitVector));

            currentChunk = chunk;
            currentSplitVector.push_back(tagRange.min);
        }
    }

    // Drain the current split vector if there are any entries left
    if (currentChunk) {
        invariant(!currentSplitVector.empty());

        splitCandidates.emplace_back(currentChunk->getShardId(),
                                     nss,
                                     cm->getVersion(),
                                     currentChunk->getLastmod(),
                                     currentChunk->getMin(),
                                     currentChunk->getMax(),
                                     std::move(currentSplitVector));
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

    auto collInfoStatus = createCollectionDistributionInfo(txn, shardStats, cm);
    if (!collInfoStatus.isOK()) {
        return collInfoStatus.getStatus();
    }

    auto collInfo = std::move(collInfoStatus.getValue());

    DistributionStatus distribution = std::move(std::get<0>(collInfo));
    ChunkMinimumsSet allChunkMinimums = std::move(std::get<1>(collInfo));

    for (const auto& tagRangeEntry : distribution.tagRanges()) {
        const auto& tagRange = tagRangeEntry.second;

        if (!allChunkMinimums.count(tagRange.min)) {
            // This tag falls somewhere at the middle of a chunk. Therefore we must skip balancing
            // this collection until it is split at the next iteration.
            //
            // TODO: We should be able to just skip chunks, which straddle tags and still make some
            // progress balancing.
            return {ErrorCodes::IllegalOperation,
                    str::stream()
                        << "Tag boundaries "
                        << tagRange.toString()
                        << " fall in the middle of an existing chunk. Balancing for collection "
                        << nss.ns()
                        << " will be postponed until the chunk is split appropriately."};
        }
    }

    return BalancerPolicy::balance(shardStats, distribution, aggressiveBalanceHint);
}

}  // namespace mongo
