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


#include "mongo/platform/basic.h"

#include "mongo/db/s/balancer/balancer_chunk_selection_policy_impl.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/db/s/sharding_config_server_parameters_gen.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/bits.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/get_stats_for_balancing_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

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
        chunk.setCollectionUUID(chunkMgr.getUUID());
        chunk.setMin(chunkEntry.getMin());
        chunk.setMax(chunkEntry.getMax());
        chunk.setJumbo(chunkEntry.isJumbo());
        chunk.setShard(chunkEntry.getShardId());
        chunk.setVersion(chunkEntry.getLastmod());

        shardToChunksMap[chunkEntry.getShardId()].push_back(chunk);

        return true;
    });

    const auto& keyPattern = chunkMgr.getShardKeyPattern().getKeyPattern();

    // Cache the collection zones
    auto swZoneInfo = ZoneInfo::getZonesForCollection(opCtx, nss, keyPattern);
    if (!swZoneInfo.isOK()) {
        return swZoneInfo.getStatus();
    }

    DistributionStatus distribution(nss, std::move(shardToChunksMap), swZoneInfo.getValue());

    return {std::move(distribution)};
}

stdx::unordered_map<NamespaceString, CollectionDataSizeInfoForBalancing>
getDataSizeInfoForCollections(OperationContext* opCtx,
                              const std::vector<CollectionType>& collections) {
    const auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
    uassertStatusOK(balancerConfig->refreshAndCheck(opCtx));

    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto shardIds = shardRegistry->getAllShardIds(opCtx);

    // Map to be returned, incrementally populated with the collected statistics
    stdx::unordered_map<NamespaceString, CollectionDataSizeInfoForBalancing> dataSizeInfoMap;

    std::vector<NamespaceWithOptionalUUID> namespacesWithUUIDsForStatsRequest;
    for (const auto& coll : collections) {
        const auto& nss = coll.getNss();
        const auto maxChunkSizeBytes =
            coll.getMaxChunkSizeBytes().value_or(balancerConfig->getMaxChunkSizeBytes());

        dataSizeInfoMap.emplace(
            nss,
            CollectionDataSizeInfoForBalancing(std::map<ShardId, int64_t>(), maxChunkSizeBytes));

        NamespaceWithOptionalUUID nssWithUUID(nss);
        nssWithUUID.setUUID(coll.getUuid());
        namespacesWithUUIDsForStatsRequest.push_back(nssWithUUID);
    }

    ShardsvrGetStatsForBalancing req{namespacesWithUUIDsForStatsRequest};
    req.setScaleFactor(1);
    const auto reqObj = req.toBSON({});

    const auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    const auto responsesFromShards =
        sharding_util::sendCommandToShards(opCtx,
                                           NamespaceString::kAdminDb.toString(),
                                           reqObj,
                                           shardIds,
                                           executor,
                                           false /* throwOnError */);

    for (auto&& response : responsesFromShards) {
        try {
            const auto& shardId = response.shardId;
            const auto errorContext =
                "Failed to get stats for balancing from shard '{}'"_format(shardId.toString());
            const auto responseValue =
                uassertStatusOKWithContext(std::move(response.swResponse), errorContext);

            const ShardsvrGetStatsForBalancingReply reply =
                ShardsvrGetStatsForBalancingReply::parse(
                    IDLParserContext("ShardsvrGetStatsForBalancingReply"),
                    std::move(responseValue.data));
            const auto collStatsFromShard = reply.getStats();

            invariant(collStatsFromShard.size() == collections.size());
            for (const auto& stats : collStatsFromShard) {
                invariant(dataSizeInfoMap.contains(stats.getNs()));
                dataSizeInfoMap.at(stats.getNs()).shardToDataSizeMap[shardId] = stats.getCollSize();
            }
        } catch (const ExceptionFor<ErrorCodes::ShardNotFound>& ex) {
            // Handle `removeShard`: skip shards removed during a balancing round
            LOGV2_DEBUG(6581603,
                        1,
                        "Skipping shard for the current balancing round",
                        "error"_attr = redact(ex));
        }
    }

    return dataSizeInfoMap;
}

CollectionDataSizeInfoForBalancing getDataSizeInfoForCollection(OperationContext* opCtx,
                                                                const NamespaceString& nss) {
    const auto coll = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss);
    std::vector<CollectionType> vec{coll};
    return std::move(getDataSizeInfoForCollections(opCtx, vec).at(nss));
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
          _chunkSplitPoints(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<SplitInfo>()) {
    }

    /**
     * Adds the specified split point to the chunk. The split points must always be within the
     * boundaries of the chunk and must come in increasing order.
     */
    void addSplitPoint(const Chunk& chunk, const BSONObj& splitPoint) {
        auto it = _chunkSplitPoints.find(chunk.getMin());
        if (it == _chunkSplitPoints.end()) {
            _chunkSplitPoints.emplace(chunk.getMin(),
                                      SplitInfo(chunk.getShardId(),
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
        SplitInfoVector splitPoints;
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
    BSONObjIndexedMap<SplitInfo> _chunkSplitPoints;
};

/**
 * Populates splitCandidates with chunk and splitPoint pairs for chunks that violate zone
 * range boundaries.
 */
void getSplitCandidatesToEnforceZoneRanges(const ChunkManager& cm,
                                           const DistributionStatus& distribution,
                                           SplitCandidatesBuffer* splitCandidates) {
    const auto& globalMax = cm.getShardKeyPattern().getKeyPattern().globalMax();

    // For each zone range, find chunks that need to be split.
    for (const auto& zoneRangeEntry : distribution.zoneRanges()) {
        const auto& zoneRange = zoneRangeEntry.second;

        const auto chunkAtZoneMin = cm.findIntersectingChunkWithSimpleCollation(zoneRange.min);
        invariant(chunkAtZoneMin.getMax().woCompare(zoneRange.min) > 0);

        if (chunkAtZoneMin.getMin().woCompare(zoneRange.min)) {
            splitCandidates->addSplitPoint(chunkAtZoneMin, zoneRange.min);
        }

        // The global max key can never fall in the middle of a chunk.
        if (!zoneRange.max.woCompare(globalMax))
            continue;

        const auto chunkAtZoneMax = cm.findIntersectingChunkWithSimpleCollation(zoneRange.max);

        // We need to check that both the chunk's minKey does not match the zone's max and also that
        // the max is not equal, which would only happen in the case of the zone ending in MaxKey.
        if (chunkAtZoneMax.getMin().woCompare(zoneRange.max) &&
            chunkAtZoneMax.getMax().woCompare(zoneRange.max)) {
            splitCandidates->addSplitPoint(chunkAtZoneMax, zoneRange.max);
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
                    "Unable to enforce zone range policy for collection {namespace}: {error}",
                    "Unable to enforce zone range policy for collection",
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
    OperationContext* opCtx, stdx::unordered_set<ShardId>* usedShards) {
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

    std::shuffle(collections.begin(), collections.end(), _random);

    static constexpr auto kStatsForBalancingBatchSize = 20;

    std::vector<CollectionType> collBatch;
    for (auto collIt = collections.begin(); collIt != collections.end();) {
        const auto& coll = *(collIt++);
        if (!coll.getAllowBalance() || !coll.getAllowMigrations() || !coll.getPermitMigrations() ||
            coll.getDefragmentCollection()) {
            LOGV2_DEBUG(5966401,
                        1,
                        "Not balancing explicitly disabled collection",
                        "namespace"_attr = coll.getNss(),
                        "allowBalance"_attr = coll.getAllowBalance(),
                        "allowMigrations"_attr = coll.getAllowMigrations(),
                        "permitMigrations"_attr = coll.getPermitMigrations(),
                        "defragmentCollection"_attr = coll.getDefragmentCollection());
        } else {
            collBatch.push_back(coll);
        }

        if (collBatch.size() < kStatsForBalancingBatchSize && collIt != collections.end()) {
            // keep Accumulating in the batch
            continue;
        }

        boost::optional<stdx::unordered_map<NamespaceString, CollectionDataSizeInfoForBalancing>>
            collsDataSizeInfo;
        if (feature_flags::gBalanceAccordingToDataSize.isEnabled(
                serverGlobalParams.featureCompatibility)) {
            collsDataSizeInfo.emplace(getDataSizeInfoForCollections(opCtx, collBatch));
        }

        for (const auto& collFromBatch : collBatch) {
            const auto& nss = collFromBatch.getNss();

            boost::optional<CollectionDataSizeInfoForBalancing> optDataSizeInfo;
            if (collsDataSizeInfo.has_value()) {
                optDataSizeInfo.emplace(std::move(collsDataSizeInfo->at(nss)));
            }

            auto candidatesStatus = _getMigrateCandidatesForCollection(
                opCtx, nss, shardStats, optDataSizeInfo, usedShards);
            if (candidatesStatus == ErrorCodes::NamespaceNotFound) {
                // Namespace got dropped before we managed to get to it, so just skip it
                continue;
            } else if (!candidatesStatus.isOK()) {
                LOGV2_WARNING(21853,
                              "Unable to balance collection",
                              "namespace"_attr = nss.ns(),
                              "error"_attr = candidatesStatus.getStatus());
                continue;
            }

            candidateChunks.insert(
                candidateChunks.end(),
                std::make_move_iterator(candidatesStatus.getValue().first.begin()),
                std::make_move_iterator(candidatesStatus.getValue().first.end()));
        }

        collBatch.clear();
    }

    return candidateChunks;
}

StatusWith<MigrateInfosWithReason> BalancerChunkSelectionPolicyImpl::selectChunksToMove(
    OperationContext* opCtx, const NamespaceString& nss) {
    auto shardStatsStatus = _clusterStats->getStats(opCtx);
    if (!shardStatsStatus.isOK()) {
        return shardStatsStatus.getStatus();
    }

    const auto& shardStats = shardStatsStatus.getValue();

    // Used to check locally if the collection exists, it should trow NamespaceNotFound if it
    // doesn't.
    Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss);

    stdx::unordered_set<ShardId> usedShards;

    boost::optional<CollectionDataSizeInfoForBalancing> optCollDataSizeInfo;
    if (feature_flags::gBalanceAccordingToDataSize.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        optCollDataSizeInfo.emplace(getDataSizeInfoForCollection(opCtx, nss));
    }

    auto candidatesStatus = _getMigrateCandidatesForCollection(
        opCtx, nss, shardStats, optCollDataSizeInfo, &usedShards);
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
                                                   distribution.getZoneForChunk(chunk));
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
        if (!distribution.zones().empty()) {
            LOGV2_WARNING(4562401,
                          "Ignoring zones for the sessions collection",
                          "zones"_attr = distribution.zones());
        }

        getSplitCandidatesForSessionsCollection(opCtx, cm, &splitCandidates);
    } else {
        getSplitCandidatesToEnforceZoneRanges(cm, distribution, &splitCandidates);
    }

    return splitCandidates.done();
}

StatusWith<MigrateInfosWithReason>
BalancerChunkSelectionPolicyImpl::_getMigrateCandidatesForCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardStatisticsVector& shardStats,
    const boost::optional<CollectionDataSizeInfoForBalancing>& collDataSizeInfo,
    stdx::unordered_set<ShardId>* usedShards) {
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

    for (const auto& zoneRangeEntry : distribution.zoneRanges()) {
        const auto& zoneRange = zoneRangeEntry.second;

        const auto chunkAtZoneMin = cm.findIntersectingChunkWithSimpleCollation(zoneRange.min);

        if (chunkAtZoneMin.getMin().woCompare(zoneRange.min)) {
            return {ErrorCodes::IllegalOperation,
                    str::stream()
                        << "Zone boundaries " << zoneRange.toString()
                        << " fall in the middle of an existing chunk "
                        << ChunkRange(chunkAtZoneMin.getMin(), chunkAtZoneMin.getMax()).toString()
                        << ". Balancing for collection " << nss.ns()
                        << " will be postponed until the chunk is split appropriately."};
        }

        // The global max key can never fall in the middle of a chunk
        if (!zoneRange.max.woCompare(shardKeyPattern.globalMax()))
            continue;

        const auto chunkAtZoneMax = cm.findIntersectingChunkWithSimpleCollation(zoneRange.max);

        // We need to check that both the chunk's minKey does not match the zone's max and also that
        // the max is not equal, which would only happen in the case of the zone ending in MaxKey.
        if (chunkAtZoneMax.getMin().woCompare(zoneRange.max) &&
            chunkAtZoneMax.getMax().woCompare(zoneRange.max)) {
            return {ErrorCodes::IllegalOperation,
                    str::stream()
                        << "Zone boundaries " << zoneRange.toString()
                        << " fall in the middle of an existing chunk "
                        << ChunkRange(chunkAtZoneMax.getMin(), chunkAtZoneMax.getMax()).toString()
                        << ". Balancing for collection " << nss.ns()
                        << " will be postponed until the chunk is split appropriately."};
        }
    }

    return BalancerPolicy::balance(
        shardStats,
        distribution,
        collDataSizeInfo,
        usedShards,
        Grid::get(opCtx)->getBalancerConfiguration()->attemptToBalanceJumboChunks());
}

}  // namespace mongo
