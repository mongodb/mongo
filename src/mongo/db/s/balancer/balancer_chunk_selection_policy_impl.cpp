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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/balancer/balancer_chunk_selection_policy_impl.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/db/s/sharding_config_server_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/bits.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/util/str.h"

namespace mongo {

using MigrateInfoVector = BalancerChunkSelectionPolicy::MigrateInfoVector;
using SplitInfoVector = BalancerChunkSelectionPolicy::SplitInfoVector;

namespace {

/**
 * Does a linear pass over the information cached in the specified chunk manager and extracts chunk
 * distribution and chunk placement information which is needed by the balancer policy.
 */
StatusWith<DistributionStatus> createCollectionDistributionStatus(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardStatisticsVector& allShards,
    const ChunkManager& chunkMgr) {
    ShardToChunksMap shardToChunksMap;

    // Makes sure there is an entry in shardToChunksMap for every shard, so empty shards will also
    // be accounted for
    for (const auto& stat : allShards) {
        shardToChunksMap[stat.shardId];
    }

    chunkMgr.forEachChunk([&](const auto& chunkEntry) {
        ChunkType chunk;
        chunk.setCollectionUUID(*chunkMgr.getUUID());
        chunk.setMin(chunkEntry.getMin());
        chunk.setMax(chunkEntry.getMax());
        chunk.setJumbo(chunkEntry.isJumbo());
        chunk.setShard(chunkEntry.getShardId());
        chunk.setVersion(chunkEntry.getLastmod());

        shardToChunksMap[chunkEntry.getShardId()].push_back(chunk);

        return true;
    });

    const auto swCollectionTags =
        Grid::get(opCtx)->catalogClient()->getTagsForCollection(opCtx, nss);
    if (!swCollectionTags.isOK()) {
        return swCollectionTags.getStatus().withContext(
            str::stream() << "Unable to load tags for collection " << nss);
    }
    const auto& collectionTags = swCollectionTags.getValue();

    DistributionStatus distribution(nss, std::move(shardToChunksMap));

    // Cache the collection tags
    const auto& keyPattern = chunkMgr.getShardKeyPattern().getKeyPattern();

    for (const auto& tag : collectionTags) {
        auto status = distribution.addRangeToZone(
            ZoneRange(keyPattern.extendRangeBound(tag.getMinKey(), false),
                      keyPattern.extendRangeBound(tag.getMaxKey(), false),
                      tag.getTag()));

        if (!status.isOK()) {
            return status;
        }
    }

    return {std::move(distribution)};
}

/**
 * Helper class used to accumulate the split points for the same chunk together so they can be
 * submitted to the shard as a single call versus multiple. This is necessary in order to avoid
 * refreshing the chunk metadata after every single split point (if done one by one), because
 * splitting a chunk does not yield the same chunk anymore.
 */
class SplitCandidatesBuffer {
    SplitCandidatesBuffer(const SplitCandidatesBuffer&) = delete;
    SplitCandidatesBuffer& operator=(const SplitCandidatesBuffer&) = delete;

public:
    SplitCandidatesBuffer(NamespaceString nss, ChunkVersion collectionVersion)
        : _nss(std::move(nss)),
          _collectionVersion(collectionVersion),
          _chunkSplitPoints(SimpleBSONObjComparator::kInstance
                                .makeBSONObjIndexedMap<BalancerChunkSelectionPolicy::SplitInfo>()) {
    }

    /**
     * Adds the specified split point to the chunk. The split points must always be within the
     * boundaries of the chunk and must come in increasing order.
     */
    void addSplitPoint(const Chunk& chunk, const BSONObj& splitPoint) {
        auto it = _chunkSplitPoints.find(chunk.getMin());
        if (it == _chunkSplitPoints.end()) {
            _chunkSplitPoints.emplace(chunk.getMin(),
                                      BalancerChunkSelectionPolicy::SplitInfo(chunk.getShardId(),
                                                                              _nss,
                                                                              _collectionVersion,
                                                                              chunk.getLastmod(),
                                                                              chunk.getMin(),
                                                                              chunk.getMax(),
                                                                              {splitPoint}));
        } else if (splitPoint.woCompare(it->second.splitKeys.back()) > 0) {
            it->second.splitKeys.push_back(splitPoint);
        } else {
            // Split points must come in order
            invariant(splitPoint.woCompare(it->second.splitKeys.back()) == 0);
        }
    }

    /**
     * May be called only once for the lifetime of the buffer. Moves the contents of the buffer into
     * a vector of split infos to be passed to the split call.
     */
    SplitInfoVector done() {
        BalancerChunkSelectionPolicy::SplitInfoVector splitPoints;
        for (const auto& entry : _chunkSplitPoints) {
            splitPoints.push_back(std::move(entry.second));
        }

        return splitPoints;
    }

private:
    // Namespace and expected collection version
    const NamespaceString _nss;
    const ChunkVersion _collectionVersion;

    // Chunk min key and split vector associated with that chunk
    BSONObjIndexedMap<BalancerChunkSelectionPolicy::SplitInfo> _chunkSplitPoints;
};

/**
 * Populates splitCandidates with chunk and splitPoint pairs for chunks that violate tag
 * range boundaries.
 */
void getSplitCandidatesToEnforceTagRanges(const ChunkManager& cm,
                                          const DistributionStatus& distribution,
                                          SplitCandidatesBuffer* splitCandidates) {
    const auto& globalMax = cm.getShardKeyPattern().getKeyPattern().globalMax();

    // For each tag range, find chunks that need to be split.
    for (const auto& tagRangeEntry : distribution.tagRanges()) {
        const auto& tagRange = tagRangeEntry.second;

        const auto chunkAtZoneMin = cm.findIntersectingChunkWithSimpleCollation(tagRange.min);
        invariant(chunkAtZoneMin.getMax().woCompare(tagRange.min) > 0);

        if (chunkAtZoneMin.getMin().woCompare(tagRange.min)) {
            splitCandidates->addSplitPoint(chunkAtZoneMin, tagRange.min);
        }

        // The global max key can never fall in the middle of a chunk.
        if (!tagRange.max.woCompare(globalMax))
            continue;

        const auto chunkAtZoneMax = cm.findIntersectingChunkWithSimpleCollation(tagRange.max);

        // We need to check that both the chunk's minKey does not match the zone's max and also that
        // the max is not equal, which would only happen in the case of the zone ending in MaxKey.
        if (chunkAtZoneMax.getMin().woCompare(tagRange.max) &&
            chunkAtZoneMax.getMax().woCompare(tagRange.max)) {
            splitCandidates->addSplitPoint(chunkAtZoneMax, tagRange.max);
        }
    }
}

/**
 * If the number of chunks as given by the ChunkManager is less than the configured minimum
 * number of chunks for the sessions collection (minNumChunksForSessionsCollection), calculates
 * split points that evenly partition the key space into N ranges (where N is
 * minNumChunksForSessionsCollection rounded up to the next power of 2), and populates
 * splitCandidates with chunk and splitPoint pairs for chunks that need to split.
 */
void getSplitCandidatesForSessionsCollection(OperationContext* opCtx,
                                             const ChunkManager& cm,
                                             SplitCandidatesBuffer* splitCandidates) {
    const auto minNumChunks = minNumChunksForSessionsCollection.load();

    if (cm.numChunks() >= minNumChunks) {
        return;
    }

    // Use the next power of 2 as the target number of chunks.
    const size_t numBits = 64 - countLeadingZeros64(minNumChunks - 1);
    const uint32_t numChunks = 1 << numBits;

    // Compute split points for _id.id that partition the UUID 128-bit data space into numChunks
    // equal ranges. Since the numChunks is a power of 2, the split points are the permutations
    // of the prefix numBits right-padded with 0's.
    std::vector<BSONObj> splitPoints;
    for (uint32_t i = 1; i < numChunks; i++) {
        // Start with a buffer of 0's.
        std::array<uint8_t, 16> buf{0b0};

        // Left-shift i to fill the remaining bits in the prefix 32 bits with 0's.
        const uint32_t high = i << (CHAR_BIT * 4 - numBits);

        // Fill the prefix 4 bytes with high's bytes.
        buf[0] = static_cast<uint8_t>(high >> CHAR_BIT * 3);
        buf[1] = static_cast<uint8_t>(high >> CHAR_BIT * 2);
        buf[2] = static_cast<uint8_t>(high >> CHAR_BIT * 1);
        buf[3] = static_cast<uint8_t>(high);

        ConstDataRange cdr(buf.data(), sizeof(buf));
        splitPoints.push_back(BSON("_id" << BSON("id" << UUID::fromCDR(cdr))));
    }

    // For each split point, find a chunk that needs to be split.
    for (auto& splitPoint : splitPoints) {
        const auto chunkAtSplitPoint = cm.findIntersectingChunkWithSimpleCollation(splitPoint);
        invariant(chunkAtSplitPoint.getMax().woCompare(splitPoint) > 0);

        if (chunkAtSplitPoint.getMin().woCompare(splitPoint)) {
            splitCandidates->addSplitPoint(chunkAtSplitPoint, splitPoint);
        }
    }

    return;
}

}  // namespace

BalancerChunkSelectionPolicyImpl::BalancerChunkSelectionPolicyImpl(ClusterStatistics* clusterStats,
                                                                   BalancerRandomSource& random)
    : _clusterStats(clusterStats), _random(random) {}

BalancerChunkSelectionPolicyImpl::~BalancerChunkSelectionPolicyImpl() = default;

StatusWith<SplitInfoVector> BalancerChunkSelectionPolicyImpl::selectChunksToSplit(
    OperationContext* opCtx) {
    auto shardStatsStatus = _clusterStats->getStats(opCtx);
    if (!shardStatsStatus.isOK()) {
        return shardStatsStatus.getStatus();
    }

    const auto& shardStats = shardStatsStatus.getValue();

    auto collections = Grid::get(opCtx)->catalogClient()->getCollections(opCtx, {});
    if (collections.empty()) {
        return SplitInfoVector{};
    }

    SplitInfoVector splitCandidates;

    std::shuffle(collections.begin(), collections.end(), _random);

    for (const auto& coll : collections) {
        const NamespaceString& nss(coll.getNss());

        auto candidatesStatus = _getSplitCandidatesForCollection(opCtx, nss, shardStats);
        if (candidatesStatus == ErrorCodes::NamespaceNotFound) {
            // Namespace got dropped before we managed to get to it, so just skip it
            continue;
        } else if (!candidatesStatus.isOK()) {
            if (nss == NamespaceString::kLogicalSessionsNamespace) {
                LOGV2_WARNING(4562402,
                              "Unable to split sessions collection chunks",
                              "error"_attr = candidatesStatus.getStatus());

            } else {
                LOGV2_WARNING(
                    21852,
                    "Unable to enforce tag range policy for collection {namespace}: {error}",
                    "Unable to enforce tag range policy for collection",
                    "namespace"_attr = nss.ns(),
                    "error"_attr = candidatesStatus.getStatus());
            }

            continue;
        }

        splitCandidates.insert(splitCandidates.end(),
                               std::make_move_iterator(candidatesStatus.getValue().begin()),
                               std::make_move_iterator(candidatesStatus.getValue().end()));
    }

    return splitCandidates;
}

StatusWith<SplitInfoVector> BalancerChunkSelectionPolicyImpl::selectChunksToSplit(
    OperationContext* opCtx, const NamespaceString& nss) {

    auto shardStatsStatus = _clusterStats->getStats(opCtx);
    if (!shardStatsStatus.isOK()) {
        return shardStatsStatus.getStatus();
    }

    const auto& shardStats = shardStatsStatus.getValue();

    return _getSplitCandidatesForCollection(opCtx, nss, shardStats);
}

StatusWith<MigrateInfoVector> BalancerChunkSelectionPolicyImpl::selectChunksToMove(
    OperationContext* opCtx) {
    auto shardStatsStatus = _clusterStats->getStats(opCtx);
    if (!shardStatsStatus.isOK()) {
        return shardStatsStatus.getStatus();
    }

    const auto& shardStats = shardStatsStatus.getValue();

    if (shardStats.size() < 2) {
        return MigrateInfoVector{};
    }

    auto collections = Grid::get(opCtx)->catalogClient()->getCollections(opCtx, {});
    if (collections.empty()) {
        return MigrateInfoVector{};
    }

    MigrateInfoVector candidateChunks;
    std::set<ShardId> usedShards;

    std::shuffle(collections.begin(), collections.end(), _random);

    for (const auto& coll : collections) {
        const NamespaceString& nss(coll.getNss());

        if (!coll.getAllowBalance() || !coll.getAllowMigrations() || !coll.getPermitMigrations()) {
            LOGV2_DEBUG(21851,
                        1,
                        "Not balancing collection {namespace}; explicitly disabled.",
                        "Not balancing explicitly disabled collection",
                        "namespace"_attr = nss,
                        "allowBalance"_attr = coll.getAllowBalance(),
                        "allowMigrations"_attr = coll.getAllowMigrations(),
                        "timeseriesFields"_attr = coll.getTimeseriesFields());
            continue;
        }

        auto candidatesStatus =
            _getMigrateCandidatesForCollection(opCtx, nss, shardStats, &usedShards);
        if (candidatesStatus == ErrorCodes::NamespaceNotFound) {
            // Namespace got dropped before we managed to get to it, so just skip it
            continue;
        } else if (!candidatesStatus.isOK()) {
            LOGV2_WARNING(21853,
                          "Unable to balance collection {namespace}: {error}",
                          "Unable to balance collection",
                          "namespace"_attr = nss.ns(),
                          "error"_attr = candidatesStatus.getStatus());
            continue;
        }

        candidateChunks.insert(candidateChunks.end(),
                               std::make_move_iterator(candidatesStatus.getValue().begin()),
                               std::make_move_iterator(candidatesStatus.getValue().end()));
    }

    return candidateChunks;
}

StatusWith<MigrateInfoVector> BalancerChunkSelectionPolicyImpl::selectChunksToMove(
    OperationContext* opCtx, const NamespaceString& nss) {
    auto shardStatsStatus = _clusterStats->getStats(opCtx);
    if (!shardStatsStatus.isOK()) {
        return shardStatsStatus.getStatus();
    }

    const auto& shardStats = shardStatsStatus.getValue();

    // Used to check locally if the collection exists, it should trow NamespaceNotFound if it
    // doesn't.
    Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss);

    std::set<ShardId> usedShards;

    auto candidatesStatus = _getMigrateCandidatesForCollection(opCtx, nss, shardStats, &usedShards);
    if (!candidatesStatus.isOK()) {
        return candidatesStatus.getStatus();
    }

    return candidatesStatus;
}

StatusWith<boost::optional<MigrateInfo>>
BalancerChunkSelectionPolicyImpl::selectSpecificChunkToMove(OperationContext* opCtx,
                                                            const NamespaceString& nss,
                                                            const ChunkType& chunk) {
    auto shardStatsStatus = _clusterStats->getStats(opCtx);
    if (!shardStatsStatus.isOK()) {
        return shardStatsStatus.getStatus();
    }

    const auto& shardStats = shardStatsStatus.getValue();

    auto routingInfoStatus =
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx, nss);
    if (!routingInfoStatus.isOK()) {
        return routingInfoStatus.getStatus();
    }

    const auto& cm = routingInfoStatus.getValue();

    const auto collInfoStatus = createCollectionDistributionStatus(opCtx, nss, shardStats, cm);
    if (!collInfoStatus.isOK()) {
        return collInfoStatus.getStatus();
    }

    const DistributionStatus& distribution = collInfoStatus.getValue();

    return BalancerPolicy::balanceSingleChunk(chunk, shardStats, distribution);
}

Status BalancerChunkSelectionPolicyImpl::checkMoveAllowed(OperationContext* opCtx,
                                                          const ChunkType& chunk,
                                                          const ShardId& newShardId) {
    auto shardStatsStatus = _clusterStats->getStats(opCtx);
    if (!shardStatsStatus.isOK()) {
        return shardStatsStatus.getStatus();
    }

    const CollectionType collection = Grid::get(opCtx)->catalogClient()->getCollection(
        opCtx, chunk.getCollectionUUID(), repl::ReadConcernLevel::kLocalReadConcern);
    const auto& nss = collection.getNss();


    auto shardStats = std::move(shardStatsStatus.getValue());

    auto routingInfoStatus =
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx, nss);
    if (!routingInfoStatus.isOK()) {
        return routingInfoStatus.getStatus();
    }

    const auto& cm = routingInfoStatus.getValue();

    const auto collInfoStatus = createCollectionDistributionStatus(opCtx, nss, shardStats, cm);
    if (!collInfoStatus.isOK()) {
        return collInfoStatus.getStatus();
    }

    const DistributionStatus& distribution = collInfoStatus.getValue();

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
    OperationContext* opCtx, const NamespaceString& nss, const ShardStatisticsVector& shardStats) {
    auto routingInfoStatus =
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx, nss);
    if (!routingInfoStatus.isOK()) {
        return routingInfoStatus.getStatus();
    }

    const auto& cm = routingInfoStatus.getValue();

    const auto collInfoStatus = createCollectionDistributionStatus(opCtx, nss, shardStats, cm);
    if (!collInfoStatus.isOK()) {
        return collInfoStatus.getStatus();
    }

    const DistributionStatus& distribution = collInfoStatus.getValue();

    // Accumulate split points for the same chunk together
    SplitCandidatesBuffer splitCandidates(nss, cm.getVersion());

    if (nss == NamespaceString::kLogicalSessionsNamespace) {
        if (!distribution.tags().empty()) {
            LOGV2_WARNING(4562401,
                          "Ignoring zones for the sessions collection",
                          "tags"_attr = distribution.tags());
        }

        getSplitCandidatesForSessionsCollection(opCtx, cm, &splitCandidates);
    } else {
        getSplitCandidatesToEnforceTagRanges(cm, distribution, &splitCandidates);
    }

    return splitCandidates.done();
}

StatusWith<MigrateInfoVector> BalancerChunkSelectionPolicyImpl::_getMigrateCandidatesForCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardStatisticsVector& shardStats,
    std::set<ShardId>* usedShards) {
    auto routingInfoStatus =
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx, nss);
    if (!routingInfoStatus.isOK()) {
        return routingInfoStatus.getStatus();
    }

    const auto& cm = routingInfoStatus.getValue();

    const auto& shardKeyPattern = cm.getShardKeyPattern().getKeyPattern();

    const auto collInfoStatus = createCollectionDistributionStatus(opCtx, nss, shardStats, cm);
    if (!collInfoStatus.isOK()) {
        return collInfoStatus.getStatus();
    }

    const DistributionStatus& distribution = collInfoStatus.getValue();

    for (const auto& tagRangeEntry : distribution.tagRanges()) {
        const auto& tagRange = tagRangeEntry.second;

        const auto chunkAtZoneMin = cm.findIntersectingChunkWithSimpleCollation(tagRange.min);

        if (chunkAtZoneMin.getMin().woCompare(tagRange.min)) {
            return {ErrorCodes::IllegalOperation,
                    str::stream()
                        << "Tag boundaries " << tagRange.toString()
                        << " fall in the middle of an existing chunk "
                        << ChunkRange(chunkAtZoneMin.getMin(), chunkAtZoneMin.getMax()).toString()
                        << ". Balancing for collection " << nss.ns()
                        << " will be postponed until the chunk is split appropriately."};
        }

        // The global max key can never fall in the middle of a chunk
        if (!tagRange.max.woCompare(shardKeyPattern.globalMax()))
            continue;

        const auto chunkAtZoneMax = cm.findIntersectingChunkWithSimpleCollation(tagRange.max);

        // We need to check that both the chunk's minKey does not match the zone's max and also that
        // the max is not equal, which would only happen in the case of the zone ending in MaxKey.
        if (chunkAtZoneMax.getMin().woCompare(tagRange.max) &&
            chunkAtZoneMax.getMax().woCompare(tagRange.max)) {
            return {ErrorCodes::IllegalOperation,
                    str::stream()
                        << "Tag boundaries " << tagRange.toString()
                        << " fall in the middle of an existing chunk "
                        << ChunkRange(chunkAtZoneMax.getMin(), chunkAtZoneMax.getMax()).toString()
                        << ". Balancing for collection " << nss.ns()
                        << " will be postponed until the chunk is split appropriately."};
        }
    }

    return BalancerPolicy::balance(
        shardStats,
        distribution,
        usedShards,
        Grid::get(opCtx)->getBalancerConfiguration()->attemptToBalanceJumboChunks());
}

}  // namespace mongo
