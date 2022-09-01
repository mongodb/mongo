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

#include "mongo/db/s/balancer/balancer_policy.h"

#include <random>

#include "mongo/db/s/balancer/type_migration.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/grid.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

MONGO_FAIL_POINT_DEFINE(balancerShouldReturnRandomMigrations);

using std::map;
using std::numeric_limits;
using std::set;
using std::string;
using std::vector;
using namespace fmt::literals;

namespace {

// This value indicates the minimum deviation shard's number of chunks need to have from the
// optimal average across all shards for a zone for a rebalancing migration to be initiated.
const size_t kDefaultImbalanceThreshold = 1;

}  // namespace

DistributionStatus::DistributionStatus(NamespaceString nss,
                                       ShardToChunksMap shardToChunksMap,
                                       ZoneInfo zoneInfo)
    : _nss(std::move(nss)),
      _shardChunks(std::move(shardToChunksMap)),
      _zoneInfo(std::move(zoneInfo)) {}

size_t DistributionStatus::totalChunks() const {
    size_t total = 0;

    for (const auto& shardChunk : _shardChunks) {
        total += shardChunk.second.size();
    }

    return total;
}

size_t DistributionStatus::totalChunksInZone(const std::string& zone) const {
    size_t total = 0;

    for (const auto& shardChunk : _shardChunks) {
        total += numberOfChunksInShardWithZone(shardChunk.first, zone);
    }

    return total;
}

size_t DistributionStatus::numberOfChunksInShard(const ShardId& shardId) const {
    const auto& shardChunks = getChunks(shardId);
    return shardChunks.size();
}

size_t DistributionStatus::numberOfChunksInShardWithZone(const ShardId& shardId,
                                                         const string& zone) const {
    const auto& shardChunks = getChunks(shardId);

    size_t total = 0;

    for (const auto& chunk : shardChunks) {
        if (zone == getZoneForChunk(chunk)) {
            total++;
        }
    }

    return total;
}

const vector<ChunkType>& DistributionStatus::getChunks(const ShardId& shardId) const {
    ShardToChunksMap::const_iterator i = _shardChunks.find(shardId);
    invariant(i != _shardChunks.end());

    return i->second;
}

Status DistributionStatus::addRangeToZone(const ZoneRange& range) {
    return _zoneInfo.addRangeToZone(range);
}

string DistributionStatus::getZoneForChunk(const ChunkType& chunk) const {
    return _zoneInfo.getZoneForChunk(chunk.getRange());
}

ZoneInfo::ZoneInfo()
    : _zoneRanges(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<ZoneRange>()) {}

Status ZoneInfo::addRangeToZone(const ZoneRange& range) {
    const auto minIntersect = _zoneRanges.upper_bound(range.min);
    const auto maxIntersect = _zoneRanges.upper_bound(range.max);

    // Check for partial overlap
    if (minIntersect != maxIntersect) {
        invariant(minIntersect != _zoneRanges.end());
        const auto& intersectingRange =
            (SimpleBSONObjComparator::kInstance.evaluate(minIntersect->second.min < range.max))
            ? minIntersect->second
            : maxIntersect->second;

        if (SimpleBSONObjComparator::kInstance.evaluate(intersectingRange.min == range.min) &&
            SimpleBSONObjComparator::kInstance.evaluate(intersectingRange.max == range.max) &&
            intersectingRange.zone == range.zone) {
            return Status::OK();
        }

        return {ErrorCodes::RangeOverlapConflict,
                str::stream() << "Zone range: " << range.toString()
                              << " is overlapping with existing: " << intersectingRange.toString()};
    }

    // Check for containment
    if (minIntersect != _zoneRanges.end()) {
        const ZoneRange& nextRange = minIntersect->second;
        if (SimpleBSONObjComparator::kInstance.evaluate(range.max > nextRange.min)) {
            invariant(SimpleBSONObjComparator::kInstance.evaluate(range.max < nextRange.max));
            return {ErrorCodes::RangeOverlapConflict,
                    str::stream() << "Zone range: " << range.toString()
                                  << " is overlapping with existing: " << nextRange.toString()};
        }
    }

    // This must be a new entry
    _zoneRanges.emplace(range.max.getOwned(), range);
    _allZones.insert(range.zone);
    return Status::OK();
}

string ZoneInfo::getZoneForChunk(const ChunkRange& chunk) const {
    const auto minIntersect = _zoneRanges.upper_bound(chunk.getMin());
    const auto maxIntersect = _zoneRanges.lower_bound(chunk.getMax());

    // We should never have a partial overlap with a chunk range. If it happens, treat it as if this
    // chunk doesn't belong to a zone
    if (minIntersect != maxIntersect) {
        return "";
    }

    if (minIntersect == _zoneRanges.end()) {
        return "";
    }

    const ZoneRange& intersectRange = minIntersect->second;

    // Check for containment
    if (SimpleBSONObjComparator::kInstance.evaluate(intersectRange.min <= chunk.getMin()) &&
        SimpleBSONObjComparator::kInstance.evaluate(chunk.getMax() <= intersectRange.max)) {
        return intersectRange.zone;
    }

    return "";
}


StatusWith<ZoneInfo> ZoneInfo::getZonesForCollection(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     const KeyPattern& keyPattern) {
    const auto swCollectionZones =
        Grid::get(opCtx)->catalogClient()->getTagsForCollection(opCtx, nss);
    if (!swCollectionZones.isOK()) {
        return swCollectionZones.getStatus().withContext(
            str::stream() << "Unable to load zones for collection " << nss);
    }
    const auto& collectionZones = swCollectionZones.getValue();

    ZoneInfo zoneInfo;

    for (const auto& zone : collectionZones) {
        auto status =
            zoneInfo.addRangeToZone(ZoneRange(keyPattern.extendRangeBound(zone.getMinKey(), false),
                                              keyPattern.extendRangeBound(zone.getMaxKey(), false),
                                              zone.getTag()));

        if (!status.isOK()) {
            return status;
        }
    }

    return zoneInfo;
}

Status BalancerPolicy::isShardSuitableReceiver(const ClusterStatistics::ShardStatistics& stat,
                                               const string& chunkZone) {
    if (stat.isDraining) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << stat.shardId << " is currently draining."};
    }

    if (!chunkZone.empty() && !stat.shardZones.count(chunkZone)) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << stat.shardId << " is not in the correct zone " << chunkZone};
    }

    return Status::OK();
}

std::tuple<ShardId, int64_t> BalancerPolicy::_getLeastLoadedReceiverShard(
    const ShardStatisticsVector& shardStats,
    const DistributionStatus& distribution,
    const boost::optional<CollectionDataSizeInfoForBalancing>& collDataSizeInfo,
    const string& zone,
    const stdx::unordered_set<ShardId>& excludedShards) {
    ShardId best;
    int64_t currentMin = numeric_limits<int64_t>::max();

    const auto shouldBalanceAccordingToDataSize = collDataSizeInfo.has_value();

    for (const auto& stat : shardStats) {
        if (excludedShards.count(stat.shardId))
            continue;

        auto status = isShardSuitableReceiver(stat, zone);
        if (!status.isOK()) {
            continue;
        }

        if (shouldBalanceAccordingToDataSize) {
            const auto& shardSizeIt = collDataSizeInfo->shardToDataSizeMap.find(stat.shardId);
            if (shardSizeIt == collDataSizeInfo->shardToDataSizeMap.end()) {
                // Skip if stats not available (may happen if add|remove shard during a round)
                continue;
            }

            int64_t shardSize = shardSizeIt->second;
            if (shardSize < currentMin) {
                best = stat.shardId;
                currentMin = shardSize;
            }
        } else {
            int64_t myChunks = distribution.numberOfChunksInShard(stat.shardId);
            if (myChunks < currentMin) {
                best = stat.shardId;
                currentMin = myChunks;
            }
        }
    }

    return {best, currentMin};
}

std::tuple<ShardId, int64_t> BalancerPolicy::_getMostOverloadedShard(
    const ShardStatisticsVector& shardStats,
    const DistributionStatus& distribution,
    const boost::optional<CollectionDataSizeInfoForBalancing>& collDataSizeInfo,
    const string& chunkZone,
    const stdx::unordered_set<ShardId>& excludedShards) {
    ShardId worst;
    long long currentMax = numeric_limits<long long>::min();

    const auto shouldBalanceAccordingToDataSize = collDataSizeInfo.has_value();

    for (const auto& stat : shardStats) {
        if (excludedShards.count(stat.shardId))
            continue;

        if (shouldBalanceAccordingToDataSize) {
            const auto& shardSizeIt = collDataSizeInfo->shardToDataSizeMap.find(stat.shardId);
            if (shardSizeIt == collDataSizeInfo->shardToDataSizeMap.end()) {
                // Skip if stats not available (may happen if add|remove shard during a round)
                continue;
            }

            const auto shardSize = shardSizeIt->second;
            if (shardSize > currentMax) {
                worst = stat.shardId;
                currentMax = shardSize;
            }
        } else {
            const unsigned shardChunkCount =
                distribution.numberOfChunksInShardWithZone(stat.shardId, chunkZone);
            if (shardChunkCount > currentMax) {
                worst = stat.shardId;
                currentMax = shardChunkCount;
            }
        }
    }

    return {worst, currentMax};
}

// Returns a random integer in [0, max) using a uniform random distribution.
int getRandomIndex(int max) {
    std::default_random_engine gen(time(nullptr));
    std::uniform_int_distribution<int> dist(0, max - 1);

    return dist(gen);
}

// Iterates through the shardStats vector starting from index until it finds an element that has > 0
// chunks. It will wrap around at the end and stop at the starting index. If no shards have chunks,
// it will return the original index value.
int getNextShardWithChunks(const ShardStatisticsVector& shardStats,
                           const DistributionStatus& distribution,
                           int index) {
    int retIndex = index;

    while (distribution.numberOfChunksInShard(shardStats[retIndex].shardId) == 0) {
        retIndex = (retIndex + 1) % shardStats.size();

        if (retIndex == index)
            return index;
    }

    return retIndex;
}

// Returns a randomly chosen pair of source -> destination shards for testing.
// The random pair is chosen by the following algorithm:
//  - create an array of indices with values [0, n)
//  - select a random index from this set
//  - advance the chosen index until we encounter a shard with chunks to move
//  - remove the chosen index from the set by swapping it with the last element
//  - select the destination index from the remaining indices
MigrateInfo chooseRandomMigration(const ShardStatisticsVector& shardStats,
                                  const DistributionStatus& distribution) {
    std::vector<int> indices(shardStats.size());

    int i = 0;
    std::generate(indices.begin(), indices.end(), [&i] { return i++; });

    int choice = getRandomIndex(indices.size());

    const int sourceIndex = getNextShardWithChunks(shardStats, distribution, indices[choice]);
    const auto& sourceShardId = shardStats[sourceIndex].shardId;
    std::swap(indices[sourceIndex], indices[indices.size() - 1]);

    choice = getRandomIndex(indices.size() - 1);
    const int destIndex = indices[choice];
    const auto& destShardId = shardStats[destIndex].shardId;

    LOGV2_DEBUG(21880,
                1,
                "balancerShouldReturnRandomMigrations: source: {fromShardId} dest: {toShardId}",
                "balancerShouldReturnRandomMigrations",
                "fromShardId"_attr = sourceShardId,
                "toShardId"_attr = destShardId);

    const auto& chunks = distribution.getChunks(sourceShardId);

    return {destShardId,
            distribution.nss(),
            chunks[getRandomIndex(chunks.size())],
            ForceJumbo::kDoNotForce};
}

MigrateInfosWithReason BalancerPolicy::balance(
    const ShardStatisticsVector& shardStats,
    const DistributionStatus& distribution,
    const boost::optional<CollectionDataSizeInfoForBalancing>& collDataSizeInfo,
    stdx::unordered_set<ShardId>* usedShards,
    bool forceJumbo) {
    vector<MigrateInfo> migrations;
    MigrationReason firstReason = MigrationReason::none;

    if (MONGO_unlikely(balancerShouldReturnRandomMigrations.shouldFail()) &&
        !distribution.nss().isConfigDB()) {
        LOGV2_DEBUG(21881, 1, "balancerShouldReturnRandomMigrations failpoint is set");

        if (shardStats.size() < 2)
            return std::make_pair(std::move(migrations), firstReason);

        migrations.push_back(chooseRandomMigration(shardStats, distribution));
        firstReason = MigrationReason::chunksImbalance;

        return std::make_pair(std::move(migrations), firstReason);
    }

    // 1) Check for shards, which are in draining mode
    {
        for (const auto& stat : shardStats) {
            if (!stat.isDraining)
                continue;

            if (usedShards->count(stat.shardId))
                continue;

            const vector<ChunkType>& chunks = distribution.getChunks(stat.shardId);

            if (chunks.empty())
                continue;

            // Now we know we need to move to chunks off this shard, but only if permitted by the
            // zones policy
            unsigned numJumboChunks = 0;

            // Since we have to move all chunks, lets just do in order
            for (const auto& chunk : chunks) {
                if (chunk.getJumbo()) {
                    numJumboChunks++;
                    continue;
                }

                const auto zone = distribution.getZoneForChunk(chunk);

                const auto [to, _] = _getLeastLoadedReceiverShard(
                    shardStats, distribution, collDataSizeInfo, zone, *usedShards);
                if (!to.isValid()) {
                    if (migrations.empty()) {
                        LOGV2_WARNING(21889,
                                      "Chunk {chunk} is on a draining shard, but no appropriate "
                                      "recipient found",
                                      "Chunk is on a draining shard, but no appropriate "
                                      "recipient found",
                                      "chunk"_attr = redact(chunk.toString()));
                    }
                    continue;
                }

                invariant(to != stat.shardId);
                migrations.emplace_back(to, distribution.nss(), chunk, ForceJumbo::kForceBalancer);
                if (firstReason == MigrationReason::none) {
                    firstReason = MigrationReason::drain;
                }
                invariant(usedShards->insert(stat.shardId).second);
                invariant(usedShards->insert(to).second);
                break;
            }

            if (migrations.empty()) {
                LOGV2_WARNING(21890,
                              "Unable to find any chunk to move from draining shard "
                              "{shardId}. numJumboChunks: {numJumboChunks}",
                              "Unable to find any chunk to move from draining shard",
                              "shardId"_attr = stat.shardId,
                              "numJumboChunks"_attr = numJumboChunks);
            }
        }
    }

    // 2) Check for chunks, which are on the wrong shard and must be moved off of it
    if (!distribution.zones().empty()) {
        for (const auto& stat : shardStats) {
            if (usedShards->count(stat.shardId))
                continue;

            const vector<ChunkType>& chunks = distribution.getChunks(stat.shardId);

            for (const auto& chunk : chunks) {
                const string zone = distribution.getZoneForChunk(chunk);

                if (zone.empty())
                    continue;

                if (stat.shardZones.count(zone))
                    continue;

                if (chunk.getJumbo()) {
                    LOGV2_WARNING(
                        21891,
                        "Chunk {chunk} violates zone {zone}, but it is jumbo and cannot be moved",
                        "Chunk violates zone, but it is jumbo and cannot be moved",
                        "chunk"_attr = redact(chunk.toString()),
                        "zone"_attr = redact(zone));

                    continue;
                }

                const auto [to, _] = _getLeastLoadedReceiverShard(
                    shardStats, distribution, collDataSizeInfo, zone, *usedShards);
                if (!to.isValid()) {
                    if (migrations.empty()) {
                        LOGV2_WARNING(21892,
                                      "Chunk {chunk} violates zone {zone}, but no appropriate "
                                      "recipient found",
                                      "Chunk violates zone, but no appropriate recipient found",
                                      "chunk"_attr = redact(chunk.toString()),
                                      "zone"_attr = redact(zone));
                    }
                    continue;
                }

                invariant(to != stat.shardId);
                migrations.emplace_back(to,
                                        distribution.nss(),
                                        chunk,
                                        forceJumbo ? ForceJumbo::kForceBalancer
                                                   : ForceJumbo::kDoNotForce);
                if (firstReason == MigrationReason::none) {
                    firstReason = MigrationReason::zoneViolation;
                }
                invariant(usedShards->insert(stat.shardId).second);
                invariant(usedShards->insert(to).second);
                break;
            }
        }
    }

    // 3) for each zone balance

    vector<string> zonesPlusEmpty(distribution.zones().begin(), distribution.zones().end());
    zonesPlusEmpty.push_back("");

    for (const auto& zone : zonesPlusEmpty) {
        size_t totalNumberOfShardsWithZone = 0;

        for (const auto& stat : shardStats) {
            if (zone.empty() || stat.shardZones.count(zone)) {
                totalNumberOfShardsWithZone++;
            }
        }

        // Skip zones which have no shards assigned to them. This situation is not harmful, but
        // should not be possible so warn the operator to correct it.
        if (totalNumberOfShardsWithZone == 0) {
            if (!zone.empty()) {
                LOGV2_WARNING(
                    21893,
                    "Zone {zone} in collection {namespace} has no assigned shards and chunks "
                    "which fall into it cannot be balanced. This should be corrected by either "
                    "assigning shards to the zone or by deleting it.",
                    "Zone in collection has no assigned shards and chunks which fall into it "
                    "cannot be balanced. This should be corrected by either assigning shards "
                    "to the zone or by deleting it.",
                    "zone"_attr = redact(zone),
                    "namespace"_attr = distribution.nss());
            }
            continue;
        }

        auto singleZoneBalance = [&]() {
            if (collDataSizeInfo.has_value()) {
                return _singleZoneBalanceBasedOnDataSize(shardStats,
                                                         distribution,
                                                         *collDataSizeInfo,
                                                         zone,
                                                         &migrations,
                                                         usedShards,
                                                         forceJumbo ? ForceJumbo::kForceBalancer
                                                                    : ForceJumbo::kDoNotForce);
            }

            return _singleZoneBalanceBasedOnChunks(shardStats,
                                                   distribution,
                                                   zone,
                                                   totalNumberOfShardsWithZone,
                                                   &migrations,
                                                   usedShards,
                                                   forceJumbo ? ForceJumbo::kForceBalancer
                                                              : ForceJumbo::kDoNotForce);
        };

        while (singleZoneBalance()) {
            if (firstReason == MigrationReason::none) {
                firstReason = MigrationReason::chunksImbalance;
            }
        }
    }

    return std::make_pair(std::move(migrations), firstReason);
}

boost::optional<MigrateInfo> BalancerPolicy::balanceSingleChunk(
    const ChunkType& chunk,
    const ShardStatisticsVector& shardStats,
    const DistributionStatus& distribution) {
    const string zone = distribution.getZoneForChunk(chunk);

    const auto [newShardId, _] = _getLeastLoadedReceiverShard(shardStats,
                                                              distribution,
                                                              boost::none /* collDataSizeInfo */,
                                                              zone,
                                                              stdx::unordered_set<ShardId>());
    if (!newShardId.isValid() || newShardId == chunk.getShard()) {
        return boost::optional<MigrateInfo>();
    }

    return MigrateInfo(newShardId, distribution.nss(), chunk, ForceJumbo::kDoNotForce);
}

bool BalancerPolicy::_singleZoneBalanceBasedOnChunks(const ShardStatisticsVector& shardStats,
                                                     const DistributionStatus& distribution,
                                                     const string& zone,
                                                     size_t totalNumberOfShardsWithZone,
                                                     vector<MigrateInfo>* migrations,
                                                     stdx::unordered_set<ShardId>* usedShards,
                                                     ForceJumbo forceJumbo) {
    // Calculate the rounded optimal number of chunks per shard
    const size_t totalNumberOfChunksInZone =
        (zone.empty() ? distribution.totalChunks() : distribution.totalChunksInZone(zone));
    const size_t idealNumberOfChunksPerShardForZone =
        (size_t)std::roundf(totalNumberOfChunksInZone / (float)totalNumberOfShardsWithZone);

    const auto [from, fromSize] =
        _getMostOverloadedShard(shardStats, distribution, boost::none, zone, *usedShards);
    if (!from.isValid())
        return false;

    const size_t max = distribution.numberOfChunksInShardWithZone(from, zone);

    // Do not use a shard if it already has less entries than the optimal per-shard chunk count
    if (max <= idealNumberOfChunksPerShardForZone)
        return false;

    const auto [to, toSize] =
        _getLeastLoadedReceiverShard(shardStats, distribution, boost::none, zone, *usedShards);
    if (!to.isValid()) {
        if (migrations->empty()) {
            LOGV2(21882, "No available shards to take chunks for zone", "zone"_attr = zone);
        }
        return false;
    }

    const size_t min = distribution.numberOfChunksInShardWithZone(to, zone);

    // Do not use a shard if it already has more entries than the optimal per-shard chunk count
    if (min >= idealNumberOfChunksPerShardForZone)
        return false;

    const size_t imbalance = max - idealNumberOfChunksPerShardForZone;

    LOGV2_DEBUG(
        21883,
        1,
        "collection: {namespace}, zone: {zone}, donor: {fromShardId} chunks on "
        " {fromShardChunkCount}, receiver: {toShardId} chunks on {toShardChunkCount}, "
        "ideal: {idealNumberOfChunksPerShardForZone}, threshold: {chunkCountImbalanceThreshold}",
        "Balancing single zone",
        "namespace"_attr = distribution.nss().ns(),
        "zone"_attr = zone,
        "fromShardId"_attr = from,
        "fromShardChunkCount"_attr = max,
        "toShardId"_attr = to,
        "toShardChunkCount"_attr = min,
        "idealNumberOfChunksPerShardForZone"_attr = idealNumberOfChunksPerShardForZone,
        "chunkCountImbalanceThreshold"_attr = kDefaultImbalanceThreshold);

    // Check whether it is necessary to balance within this zone
    if (imbalance < kDefaultImbalanceThreshold)
        return false;

    const vector<ChunkType>& chunks = distribution.getChunks(from);

    unsigned numJumboChunks = 0;

    for (const auto& chunk : chunks) {
        if (distribution.getZoneForChunk(chunk) != zone)
            continue;

        if (chunk.getJumbo()) {
            numJumboChunks++;
            continue;
        }

        migrations->emplace_back(to, distribution.nss(), chunk, forceJumbo);
        invariant(usedShards->insert(chunk.getShard()).second);
        invariant(usedShards->insert(to).second);
        return true;
    }

    if (numJumboChunks) {
        LOGV2_WARNING(
            21894,
            "Shard: {shardId}, collection: {namespace} has only jumbo chunks for "
            "zone \'{zone}\' and cannot be balanced. Jumbo chunks count: {numJumboChunks}",
            "Shard has only jumbo chunks for and cannot be balanced",
            "shardId"_attr = from,
            "namespace"_attr = distribution.nss().ns(),
            "zone"_attr = zone,
            "numJumboChunks"_attr = numJumboChunks);
    }

    return false;
}

bool BalancerPolicy::_singleZoneBalanceBasedOnDataSize(
    const ShardStatisticsVector& shardStats,
    const DistributionStatus& distribution,
    const CollectionDataSizeInfoForBalancing& collDataSizeInfo,
    const string& zone,
    vector<MigrateInfo>* migrations,
    stdx::unordered_set<ShardId>* usedShards,
    ForceJumbo forceJumbo) {
    const auto [from, fromSize] =
        _getMostOverloadedShard(shardStats, distribution, collDataSizeInfo, zone, *usedShards);
    if (!from.isValid())
        return false;

    const auto [to, toSize] =
        _getLeastLoadedReceiverShard(shardStats, distribution, collDataSizeInfo, zone, *usedShards);
    if (!to.isValid()) {
        if (migrations->empty()) {
            LOGV2(6581600, "No available shards to take chunks for zone", "zone"_attr = zone);
        }
        return false;
    }

    if (from == to) {
        return false;
    }

    LOGV2_DEBUG(6581601,
                1,
                "Balancing single zone",
                "namespace"_attr = distribution.nss().ns(),
                "zone"_attr = zone,
                "fromShardId"_attr = from,
                "fromShardDataSize"_attr = fromSize,
                "toShardId"_attr = to,
                "toShardDataSize"_attr = toSize,
                "maxChunkSizeBytes"_attr = collDataSizeInfo.maxChunkSizeBytes);

    if (fromSize - toSize < 3 * collDataSizeInfo.maxChunkSizeBytes) {
        // Do not balance if the collection's size differs too few between the chosen shards
        return false;
    }

    const vector<ChunkType>& chunks = distribution.getChunks(from);

    unsigned numJumboChunks = 0;

    for (const auto& chunk : chunks) {
        if (distribution.getZoneForChunk(chunk) != zone)
            continue;

        if (chunk.getJumbo()) {
            numJumboChunks++;
            continue;
        }

        migrations->emplace_back(to,
                                 chunk.getShard(),
                                 distribution.nss(),
                                 chunk.getCollectionUUID(),
                                 chunk.getMin(),
                                 boost::none /* call moveRange*/,
                                 chunk.getVersion(),
                                 forceJumbo,
                                 collDataSizeInfo.maxChunkSizeBytes);
        invariant(usedShards->insert(chunk.getShard()).second);
        invariant(usedShards->insert(to).second);
        return true;
    }

    if (numJumboChunks) {
        LOGV2_WARNING(6581602,
                      "Shard has only jumbo chunks for this collection and cannot be balanced",
                      "namespace"_attr = distribution.nss().ns(),
                      "shardId"_attr = from,
                      "zone"_attr = zone,
                      "numJumboChunks"_attr = numJumboChunks);
    }

    return false;
}

ZoneRange::ZoneRange(const BSONObj& a_min, const BSONObj& a_max, const std::string& _zone)
    : min(a_min.getOwned()), max(a_max.getOwned()), zone(_zone) {}

string ZoneRange::toString() const {
    return str::stream() << min << " -->> " << max << "  on  " << zone;
}

MigrateInfo::MigrateInfo(const ShardId& a_to,
                         const NamespaceString& a_nss,
                         const ChunkType& a_chunk,
                         const ForceJumbo a_forceJumbo)
    : nss(a_nss), uuid(a_chunk.getCollectionUUID()) {
    invariant(a_to.isValid());

    to = a_to;

    from = a_chunk.getShard();
    minKey = a_chunk.getMin();
    maxKey = a_chunk.getMax();
    version = a_chunk.getVersion();
    forceJumbo = a_forceJumbo;
}

MigrateInfo::MigrateInfo(const ShardId& a_to,
                         const ShardId& a_from,
                         const NamespaceString& a_nss,
                         const UUID& a_uuid,
                         const BSONObj& a_min,
                         const boost::optional<BSONObj>& a_max,
                         const ChunkVersion& a_version,
                         const ForceJumbo a_forceJumbo,
                         boost::optional<int64_t> maxChunkSizeBytes)
    : nss(a_nss),
      uuid(a_uuid),
      minKey(a_min),
      maxKey(a_max),
      version(a_version),
      forceJumbo(a_forceJumbo),
      optMaxChunkSizeBytes(maxChunkSizeBytes) {
    invariant(a_to.isValid());
    invariant(a_from.isValid());

    to = a_to;
    from = a_from;
}

std::string MigrateInfo::getName() const {
    // Generates a unique name for a MigrateInfo based on the namespace and the lower bound of the
    // chunk being moved.
    StringBuilder buf;
    buf << uuid << "-";

    BSONObjIterator i(minKey);
    while (i.more()) {
        BSONElement e = i.next();
        buf << e.fieldName() << "_" << e.toString(false, true);
    }

    return buf.str();
}

BSONObj MigrateInfo::getMigrationTypeQuery() const {
    // Generates a query object for a single MigrationType based on the namespace and the lower
    // bound of the chunk being moved.
    return BSON(MigrationType::ns(nss.ns()) << MigrationType::min(minKey));
}

string MigrateInfo::toString() const {
    return str::stream() << uuid << ": [" << minKey << ", " << maxKey << "), from " << from
                         << ", to " << to;
}

SplitInfo::SplitInfo(const ShardId& inShardId,
                     const NamespaceString& inNss,
                     const ChunkVersion& inCollectionVersion,
                     const ChunkVersion& inChunkVersion,
                     const BSONObj& inMinKey,
                     const BSONObj& inMaxKey,
                     std::vector<BSONObj> inSplitKeys)
    : shardId(inShardId),
      nss(inNss),
      collectionVersion(inCollectionVersion),
      chunkVersion(inChunkVersion),
      minKey(inMinKey),
      maxKey(inMaxKey),
      splitKeys(std::move(inSplitKeys)) {}

std::string SplitInfo::toString() const {
    StringBuilder splitKeysBuilder;
    for (const auto& splitKey : splitKeys) {
        splitKeysBuilder << splitKey.toString() << ", ";
    }

    return "Splitting chunk in {} [ {}, {} ), residing on {} at [ {} ] with version {} and collection version {}"_format(
        nss.ns(),
        minKey.toString(),
        maxKey.toString(),
        shardId.toString(),
        splitKeysBuilder.str(),
        chunkVersion.toString(),
        collectionVersion.toString());
}

SplitInfoWithKeyPattern::SplitInfoWithKeyPattern(const ShardId& shardId,
                                                 const NamespaceString& nss,
                                                 const ChunkVersion& collectionVersion,
                                                 const BSONObj& minKey,
                                                 const BSONObj& maxKey,
                                                 std::vector<BSONObj> splitKeys,
                                                 const UUID& uuid,
                                                 const BSONObj& keyPattern)
    : info(SplitInfo(
          shardId, nss, collectionVersion, ChunkVersion(), minKey, maxKey, std::move(splitKeys))),
      uuid(uuid),
      keyPattern(keyPattern) {}

AutoSplitVectorInfo::AutoSplitVectorInfo(const ShardId& shardId,
                                         const NamespaceString& nss,
                                         const UUID& uuid,
                                         const ChunkVersion& collectionVersion,
                                         const BSONObj& keyPattern,
                                         const BSONObj& minKey,
                                         const BSONObj& maxKey,
                                         long long maxChunkSizeBytes)
    : shardId(shardId),
      nss(nss),
      uuid(uuid),
      collectionVersion(collectionVersion),
      keyPattern(keyPattern),
      minKey(minKey),
      maxKey(maxKey),
      maxChunkSizeBytes(maxChunkSizeBytes) {}

MergeInfo::MergeInfo(const ShardId& shardId,
                     const NamespaceString& nss,
                     const UUID& uuid,
                     const ChunkVersion& collectionVersion,
                     const ChunkRange& chunkRange)
    : shardId(shardId),
      nss(nss),
      uuid(uuid),
      collectionVersion(collectionVersion),
      chunkRange(chunkRange) {}

std::string MergeInfo::toString() const {
    return "Merging chunk range {} in {} residing on {} with collection version {}"_format(
        chunkRange.toString(), nss.toString(), shardId.toString(), collectionVersion.toString());
}

DataSizeInfo::DataSizeInfo(const ShardId& shardId,
                           const NamespaceString& nss,
                           const UUID& uuid,
                           const ChunkRange& chunkRange,
                           const ShardVersion& version,
                           const KeyPattern& keyPattern,
                           bool estimatedValue)
    : shardId(shardId),
      nss(nss),
      uuid(uuid),
      chunkRange(chunkRange),
      version(version),
      keyPattern(keyPattern),
      estimatedValue(estimatedValue) {}

}  // namespace mongo
