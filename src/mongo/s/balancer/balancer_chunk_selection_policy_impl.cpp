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
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using MigrateInfoVector = BalancerChunkSelectionPolicy::MigrateInfoVector;
using SplitInfoVector = BalancerChunkSelectionPolicy::SplitInfoVector;
using std::shared_ptr;
using std::unique_ptr;
using std::vector;

BalancerChunkSelectionPolicyImpl::BalancerChunkSelectionPolicyImpl(
    std::unique_ptr<ClusterStatistics> clusterStats)
    : _clusterStats(std::move(clusterStats)) {}

BalancerChunkSelectionPolicyImpl::~BalancerChunkSelectionPolicyImpl() = default;

StatusWith<SplitInfoVector> BalancerChunkSelectionPolicyImpl::selectChunksToSplit(
    OperationContext* txn) {
    vector<CollectionType> collections;

    Status collsStatus =
        grid.catalogManager(txn)->getCollections(txn, nullptr, &collections, nullptr);
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
        grid.catalogManager(txn)->getCollections(txn, nullptr, &collections, nullptr);
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

StatusWith<SplitInfoVector> BalancerChunkSelectionPolicyImpl::_getSplitCandidatesForCollection(
    OperationContext* txn, const NamespaceString& nss) {
    // Ensure the database exists
    auto dbStatus = Grid::get(txn)->catalogCache()->getDatabase(txn, nss.db().toString());
    if (!dbStatus.isOK()) {
        return {dbStatus.getStatus().code(),
                str::stream() << "Database " << nss.ns() << " was not found due to "
                              << dbStatus.getStatus().toString()};
    }

    shared_ptr<DBConfig> db = dbStatus.getValue();
    invariant(db);

    // Ensure that the collection is sharded
    shared_ptr<ChunkManager> cm = db->getChunkManagerIfExists(txn, nss.ns(), true);
    if (!cm) {
        return {ErrorCodes::NamespaceNotSharded,
                str::stream() << "Collection " << nss.ns() << " does not exist or is not sharded."};
    }

    if (cm->getChunkMap().empty()) {
        return {ErrorCodes::NamespaceNotSharded,
                str::stream() << "Collection " << nss.ns() << " does not have any chunks."};
    }

    vector<TagsType> collectionTags;
    Status tagsStatus =
        grid.catalogManager(txn)->getTagsForCollection(txn, nss.ns(), &collectionTags);
    if (!tagsStatus.isOK()) {
        return {tagsStatus.code(),
                str::stream() << "Unable to load tags for collection " << nss.ns() << " due to "
                              << tagsStatus.toString()};
    }

    std::set<BSONObj> allChunkMinimums;

    for (const auto& entry : cm->getChunkMap()) {
        const auto& chunkEntry = entry.second;
        allChunkMinimums.insert(chunkEntry->getMin());
    }

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
    // Ensure the database exists
    auto dbStatus = Grid::get(txn)->catalogCache()->getDatabase(txn, nss.db().toString());
    if (!dbStatus.isOK()) {
        return {dbStatus.getStatus().code(),
                str::stream() << "Database " << nss.ns() << " was not found due to "
                              << dbStatus.getStatus().toString()};
    }

    shared_ptr<DBConfig> db = dbStatus.getValue();
    invariant(db);

    // Ensure that the collection is sharded
    shared_ptr<ChunkManager> cm = db->getChunkManagerIfExists(txn, nss.ns(), true);
    if (!cm) {
        return {ErrorCodes::NamespaceNotSharded,
                str::stream() << "Collection " << nss.ns() << " does not exist or is not sharded."};
    }

    if (cm->getChunkMap().empty()) {
        return {ErrorCodes::NamespaceNotSharded,
                str::stream() << "Collection " << nss.ns() << " does not have any chunks."};
    }

    ShardToChunksMap shardToChunksMap;
    std::set<BSONObj> allChunkMinimums;

    for (const auto& entry : cm->getChunkMap()) {
        const auto& chunkEntry = entry.second;

        ChunkType chunk;
        chunk.setMin(chunkEntry->getMin());
        chunk.setMax(chunkEntry->getMax());
        chunk.setJumbo(chunkEntry->isJumbo());

        shardToChunksMap[chunkEntry->getShardId()].push_back(chunk);
        allChunkMinimums.insert(chunkEntry->getMin());
    }

    for (const auto& stat : shardStats) {
        // This loop just makes sure there is an entry in shardToChunksMap for every shard, which we
        // plan to consider.
        shardToChunksMap[stat.shardId];
    }

    DistributionStatus distStatus(shardStats, shardToChunksMap);
    {
        vector<TagsType> collectionTags;
        Status status =
            grid.catalogManager(txn)->getTagsForCollection(txn, nss.ns(), &collectionTags);
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
                            << "Tag boundaries " << tagInfo.toString()
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
