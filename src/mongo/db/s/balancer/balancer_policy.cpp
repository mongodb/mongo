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

#include "mongo/db/s/balancer/balancer_policy.h"

#include <random>

#include "mongo/db/s/balancer/type_migration.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(balancerShouldReturnRandomMigrations);

using std::map;
using std::numeric_limits;
using std::set;
using std::string;
using std::vector;

namespace {

// This value indicates the minimum deviation shard's number of chunks need to have from the
// optimal average across all shards for a zone for a rebalancing migration to be initiated.
const size_t kDefaultImbalanceThreshold = 1;

ChunkType makeChunkType(const NamespaceString& nss, const Chunk& chunk) {
    ChunkType ct{nss, chunk.getRange(), chunk.getLastmod(), chunk.getShardId()};
    ct.setJumbo(chunk.isJumbo());
    return ct;
}

/**
 * Return a vector of zones after they have been normalized according to the given chunk
 * configuration.
 *
 * If a zone covers only partially a chunk, boundaries of that zone will be shrank so that the
 * normalized zone won't overlap with that chunk. The boundaries of a normalized zone will never
 * fall in the middle of a chunk.
 *
 * Additionally the vector will contain also zones for the "NoZone",
 */
std::vector<ZoneRange> normalizeZones(const ChunkManager* cm, const ZoneInfo& zoneInfo) {
    std::vector<ZoneRange> normalizedRanges;

    const auto& globalMaxKey = cm->getShardKeyPattern().getKeyPattern().globalMax();
    auto lastMax = cm->getShardKeyPattern().getKeyPattern().globalMin();

    for (const auto& zoneRangeIt : zoneInfo.zoneRanges()) {
        const auto& zoneRange = zoneRangeIt.second;
        const auto& minChunk = cm->findIntersectingChunkWithSimpleCollation(zoneRange.min);
        const auto gtMin =
            SimpleBSONObjComparator::kInstance.evaluate(zoneRange.min > minChunk.getMin());
        const auto& normalizedMin = gtMin ? minChunk.getMax() : zoneRange.min;

        const auto& normalizedMax = [&] {
            // ChunkManager::findIntersectingChun does not work with maxKey, thus we need to
            // explicitely check zoneRange max.
            if (SimpleBSONObjComparator::kInstance.evaluate(zoneRange.max == globalMaxKey)) {
                return zoneRange.max;
            }
            const auto& maxChunk = cm->findIntersectingChunkWithSimpleCollation(zoneRange.max);
            const auto gtMax =
                SimpleBSONObjComparator::kInstance.evaluate(zoneRange.max > maxChunk.getMin()) &&
                SimpleBSONObjComparator::kInstance.evaluate(
                    zoneRange.max != cm->getShardKeyPattern().getKeyPattern().globalMax());
            return gtMax ? maxChunk.getMin() : zoneRange.max;
        }();


        if (SimpleBSONObjComparator::kInstance.evaluate(normalizedMin == normalizedMax)) {
            // This normalised zone has a length of zero, therefore can't contain any chunks so we
            // can ignore it
            continue;
        }

        if (SimpleBSONObjComparator::kInstance.evaluate(normalizedMin != lastMax)) {
            // The zone is not contiguous with the previous one so we add a kNoZoneRange
            // does not fully contain any chunk so we will ignore it
            normalizedRanges.emplace_back(lastMax, normalizedMin, ZoneInfo::kNoZoneName);
        }

        normalizedRanges.emplace_back(normalizedMin, normalizedMax, zoneRange.zone);
        lastMax = normalizedMax;
    }

    if (SimpleBSONObjComparator::kInstance.evaluate(lastMax != globalMaxKey)) {
        normalizedRanges.emplace_back(lastMax, globalMaxKey, ZoneInfo::kNoZoneName);
    }
    return normalizedRanges;
}

}  // namespace

DistributionStatus::DistributionStatus(NamespaceString nss,
                                       ZoneInfo zoneInfo,
                                       const ChunkManager* chunkMngr)
    : _nss(std::move(nss)), _zoneInfo(std::move(zoneInfo)), _chunkMngr(chunkMngr) {

    _normalizedZones = normalizeZones(_chunkMngr, _zoneInfo);

    for (size_t zoneRangeIdx = 0; zoneRangeIdx < _normalizedZones.size(); zoneRangeIdx++) {
        const auto& zoneRange = _normalizedZones[zoneRangeIdx];
        chunkMngr->forEachOverlappingChunk(
            zoneRange.min, zoneRange.max, false /* isMaxInclusive */, [&](const auto& chunkInfo) {
                auto [zoneIt, created] =
                    _shardZoneInfoMap[chunkInfo.getShardId().toString()].try_emplace(
                        zoneRange.zone, 1 /* numChunks */, zoneRangeIdx, chunkInfo.getMin());

                if (!created) {
                    ++(zoneIt->second.numChunks);
                }
                return true;
            });
    }
}

size_t DistributionStatus::totalChunksWithTag(const std::string& tag) const {
    size_t total = 0;
    for (const auto& [_, shardZoneInfo] : _shardZoneInfoMap) {
        const auto& zoneIt = shardZoneInfo.find(tag);
        if (zoneIt != shardZoneInfo.end()) {
            total += zoneIt->second.numChunks;
        }
    }
    return total;
}

size_t DistributionStatus::numberOfChunksInShard(const ShardId& shardId) const {
    const auto shardZonesIt = _shardZoneInfoMap.find(shardId.toString());
    if (shardZonesIt == _shardZoneInfoMap.end()) {
        return 0;
    }
    size_t total = 0;
    for (const auto& [_, shardZoneInfo] : shardZonesIt->second) {
        total += shardZoneInfo.numChunks;
    }
    return total;
}

size_t DistributionStatus::numberOfChunksInShardWithTag(const ShardId& shardId,
                                                        const string& tag) const {
    const auto shardZonesIt = _shardZoneInfoMap.find(shardId.toString());
    if (shardZonesIt == _shardZoneInfoMap.end()) {
        return 0;
    }
    const auto& shardTags = shardZonesIt->second;

    const auto& zoneIt = shardTags.find(tag);
    if (zoneIt == shardTags.end()) {
        return 0;
    }
    return zoneIt->second.numChunks;
}

string DistributionStatus::getTagForRange(const ChunkRange& range) const {
    return _zoneInfo.getZoneForChunk(range);
}

const StringMap<ShardZoneInfo>& DistributionStatus::getZoneInfoForShard(
    const ShardId& shardId) const {
    static const StringMap<ShardZoneInfo> emptyMap;
    const auto shardZonesIt = _shardZoneInfoMap.find(shardId.toString());
    if (shardZonesIt == _shardZoneInfoMap.end()) {
        return emptyMap;
    }
    return shardZonesIt->second;
}

const string ZoneInfo::kNoZoneName = "";

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
    // chunk doesn't belong to a tag
    if (minIntersect != maxIntersect) {
        return ZoneInfo::kNoZoneName;
    }

    if (minIntersect == _zoneRanges.end()) {
        return ZoneInfo::kNoZoneName;
    }

    const ZoneRange& intersectRange = minIntersect->second;

    // Check for containment
    if (SimpleBSONObjComparator::kInstance.evaluate(intersectRange.min <= chunk.getMin()) &&
        SimpleBSONObjComparator::kInstance.evaluate(chunk.getMax() <= intersectRange.max)) {
        return intersectRange.zone;
    }

    return ZoneInfo::kNoZoneName;
}

void DistributionStatus::report(BSONObjBuilder* builder) const {
    builder->append("ns", _nss.ns());

    // Report all shards
    BSONArrayBuilder shardArr(builder->subarrayStart("shards"));
    for (const auto& [shardId, zoneInfoMap] : _shardZoneInfoMap) {
        BSONObjBuilder shardEntry(shardArr.subobjStart());
        shardEntry.append("name", shardId);

        BSONObjBuilder tagsObj(shardEntry.subobjStart("tags"));
        for (const auto& [tagName, shardZoneInfo] : zoneInfoMap) {
            tagsObj.appendNumber(tagName, static_cast<long long>(shardZoneInfo.numChunks));
        }
        tagsObj.doneFast();

        shardEntry.doneFast();
    }
    shardArr.doneFast();

    // Report all tags
    BSONArrayBuilder tagsArr(builder->subarrayStart("tags"));
    tagsArr.append(_zoneInfo.allZones());
    tagsArr.doneFast();

    // Report all tag ranges
    BSONArrayBuilder tagRangesArr(builder->subarrayStart("tagRanges"));
    for (const auto& tagRange : _zoneInfo.zoneRanges()) {
        BSONObjBuilder tagRangeEntry(tagRangesArr.subobjStart());
        tagRangeEntry.append("tag", tagRange.second.zone);
        tagRangeEntry.append("mapKey", tagRange.first);
        tagRangeEntry.append("min", tagRange.second.min);
        tagRangeEntry.append("max", tagRange.second.max);
        tagRangeEntry.doneFast();
    }
    tagRangesArr.doneFast();
}

string DistributionStatus::toString() const {
    BSONObjBuilder builder;
    report(&builder);

    return builder.obj().toString();
}

Status BalancerPolicy::isShardSuitableReceiver(const ClusterStatistics::ShardStatistics& stat,
                                               const string& chunkTag) {
    if (stat.isSizeMaxed()) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << stat.shardId << " has reached its maximum storage size."};
    }

    if (stat.isDraining) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << stat.shardId << " is currently draining."};
    }

    if (chunkTag != ZoneInfo::kNoZoneName && !stat.shardTags.count(chunkTag)) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << stat.shardId << " is not in the correct zone " << chunkTag};
    }

    return Status::OK();
}

ShardId BalancerPolicy::_getLeastLoadedReceiverShard(
    const ShardStatisticsVector& shardStats,
    const DistributionStatus& distribution,
    const string& tag,
    const stdx::unordered_set<ShardId>& availableShards) {
    ShardId best;
    unsigned minChunks = numeric_limits<unsigned>::max();

    for (const auto& stat : shardStats) {
        if (!availableShards.count(stat.shardId))
            continue;

        auto status = isShardSuitableReceiver(stat, tag);
        if (!status.isOK()) {
            continue;
        }

        unsigned myChunks = distribution.numberOfChunksInShard(stat.shardId);
        if (myChunks >= minChunks) {
            continue;
        }

        best = stat.shardId;
        minChunks = myChunks;
    }

    return best;
}

ShardId BalancerPolicy::_getMostOverloadedShard(
    const ShardStatisticsVector& shardStats,
    const DistributionStatus& distribution,
    const string& chunkTag,
    const stdx::unordered_set<ShardId>& availableShards) {
    ShardId worst;
    unsigned maxChunks = 0;

    for (const auto& stat : shardStats) {
        if (!availableShards.count(stat.shardId))
            continue;

        const unsigned shardChunkCount =
            distribution.numberOfChunksInShardWithTag(stat.shardId, chunkTag);
        if (shardChunkCount <= maxChunks)
            continue;

        worst = stat.shardId;
        maxChunks = shardChunkCount;
    }

    return worst;
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

    const auto& randomChunk = [&] {
        const auto numChunksOnSourceShard = distribution.numberOfChunksInShard(sourceShardId);
        const auto rndChunkIdx = getRandomIndex(numChunksOnSourceShard);
        ChunkType rndChunk;

        int idx{0};
        distribution.getChunkManager()->forEachChunk([&](const auto& chunk) {
            if (chunk.getShardId() == sourceShardId && idx++ == rndChunkIdx) {
                rndChunk = makeChunkType(distribution.nss(), chunk);
                rndChunk.setJumbo(chunk.isJumbo());
                return false;
            }
            return true;
        });

        invariant(rndChunk.getShard().isValid());
        return rndChunk;
    }();

    return {destShardId,
            randomChunk,
            MoveChunkRequest::ForceJumbo::kDoNotForce,
            MigrateInfo::chunksImbalance};
}

vector<MigrateInfo> BalancerPolicy::balance(const ShardStatisticsVector& shardStats,
                                            const DistributionStatus& distribution,
                                            stdx::unordered_set<ShardId>* availableShards,
                                            bool forceJumbo) {
    vector<MigrateInfo> migrations;

    if (MONGO_unlikely(balancerShouldReturnRandomMigrations.shouldFail()) &&
        !distribution.nss().isConfigDB()) {
        LOGV2_DEBUG(21881, 1, "balancerShouldReturnRandomMigrations failpoint is set");

        if (shardStats.size() < 2)
            return migrations;

        migrations.push_back(chooseRandomMigration(shardStats, distribution));

        return migrations;
    }

    // 1) Check for shards, which are in draining mode
    {
        for (const auto& stat : shardStats) {
            if (!stat.isDraining)
                continue;

            if (!availableShards->count(stat.shardId))
                continue;

            // Now we know we need to move chunks off this shard, but only if permitted by the
            // tags policy
            unsigned numJumboChunks = 0;

            const auto& shardZones = distribution.getZoneInfoForShard(stat.shardId);
            for (const auto& shardZone : shardZones) {
                const auto& zoneName = shardZone.first;

                const auto chunkFoundForShard = !distribution.forEachChunkOnShardInZone(
                    stat.shardId, zoneName, [&](const auto& chunk) {
                        if (chunk.isJumbo()) {
                            numJumboChunks++;
                            return true;  // continue
                        }

                        const ShardId to = _getLeastLoadedReceiverShard(
                            shardStats, distribution, zoneName, *availableShards);
                        if (!to.isValid()) {
                            if (migrations.empty()) {
                                LOGV2_DEBUG(
                                    21889,
                                    3,
                                    "Chunk {chunk} is on a draining shard, but no appropriate "
                                    "recipient found",
                                    "Chunk is on a draining shard, but no appropriate "
                                    "recipient found",
                                    "chunk"_attr = redact(
                                        makeChunkType(distribution.nss(), chunk).toString()));
                            }
                            return true;  // continue
                        }
                        invariant(to != stat.shardId);

                        migrations.emplace_back(to,
                                                makeChunkType(distribution.nss(), chunk),
                                                MoveChunkRequest::ForceJumbo::kForceBalancer,
                                                MigrateInfo::drain);

                        invariant(availableShards->erase(stat.shardId));
                        invariant(availableShards->erase(to));
                        return false;  // break
                    });

                if (chunkFoundForShard) {
                    break;
                }
            }

            if (migrations.empty()) {
                LOGV2_WARNING(21890,
                              "Unable to find any chunk to move from draining shard "
                              "{shardId}. numJumboChunks: {numJumboChunks}",
                              "Unable to find any chunk to move from draining shard",
                              "shardId"_attr = stat.shardId,
                              "numJumboChunks"_attr = numJumboChunks);
            }

            if (availableShards->size() < 2) {
                return migrations;
            }
        }
    }

    // 2) Check for chunks, which are on the wrong shard and must be moved off of it
    if (!distribution.tags().empty()) {
        for (const auto& stat : shardStats) {

            if (!availableShards->count(stat.shardId))
                continue;

            const auto& shardZones = distribution.getZoneInfoForShard(stat.shardId);
            for (const auto& shardZone : shardZones) {
                const auto& zoneName = shardZone.first;

                if (zoneName == ZoneInfo::kNoZoneName)
                    continue;

                if (stat.shardTags.count(zoneName))
                    continue;

                const auto chunkFoundForShard = !distribution.forEachChunkOnShardInZone(
                    stat.shardId, zoneName, [&](const auto& chunk) {
                        if (chunk.isJumbo()) {
                            LOGV2_WARNING(21891,
                                          "Chunk {chunk} violates zone {zone}, but it "
                                          "is jumbo and "
                                          "cannot be "
                                          "moved",
                                          "Chunk violates zone, but it is jumbo and "
                                          "cannot be moved",
                                          "chunk"_attr = redact(
                                              makeChunkType(distribution.nss(), chunk).toString()),
                                          "zone"_attr = redact(zoneName));
                            return true;  // continue
                        }

                        const ShardId to = _getLeastLoadedReceiverShard(
                            shardStats, distribution, zoneName, *availableShards);
                        if (!to.isValid()) {
                            if (migrations.empty()) {
                                LOGV2_DEBUG(
                                    21892,
                                    3,
                                    "Chunk {chunk} violates zone {zone}, but no appropriate "
                                    "recipient found",
                                    "Chunk violates zone, but no appropriate recipient found",
                                    "chunk"_attr =
                                        redact(makeChunkType(distribution.nss(), chunk).toString()),
                                    "zone"_attr = redact(zoneName));
                            }
                            return true;  // continue
                        }
                        invariant(to != stat.shardId);

                        migrations.emplace_back(to,
                                                makeChunkType(distribution.nss(), chunk),
                                                forceJumbo
                                                    ? MoveChunkRequest::ForceJumbo::kForceBalancer
                                                    : MoveChunkRequest::ForceJumbo::kDoNotForce,
                                                MigrateInfo::zoneViolation);

                        invariant(availableShards->erase(stat.shardId));
                        invariant(availableShards->erase(to));
                        return false;  // break
                    });

                if (chunkFoundForShard) {
                    break;
                }
            }
            if (availableShards->size() < 2) {
                return migrations;
            }
        }
    }

    // 3) for each tag balance

    vector<string> tagsPlusEmpty(distribution.tags().begin(), distribution.tags().end());
    tagsPlusEmpty.push_back(ZoneInfo::kNoZoneName);

    for (const auto& tag : tagsPlusEmpty) {

        const auto totalNumberOfChunksWithTag = [&] {
            if (tag == ZoneInfo::kNoZoneName) {
                return static_cast<size_t>(distribution.getChunkManager()->numChunks());
            }
            return distribution.totalChunksWithTag(tag);
        }();

        const auto totalNumberOfShardsWithTag = [&] {
            if (tag == ZoneInfo::kNoZoneName) {
                return shardStats.size();
            }

            size_t numShardsWithTag{0};
            for (const auto& stat : shardStats) {
                if (stat.shardTags.count(tag)) {
                    numShardsWithTag++;
                }
            }
            return numShardsWithTag;
        }();

        // Skip zones which have no shards assigned to them. This situation is not harmful, but
        // should not be possible so warn the operator to correct it.
        if (totalNumberOfShardsWithTag == 0) {
            if (tag != ZoneInfo::kNoZoneName) {
                LOGV2_WARNING(
                    21893,
                    "Zone {zone} in collection {namespace} has no assigned shards and chunks "
                    "which fall into it cannot be balanced. This should be corrected by either "
                    "assigning shards to the zone or by deleting it.",
                    "Zone in collection has no assigned shards and chunks which fall into it "
                    "cannot be balanced. This should be corrected by either assigning shards "
                    "to the zone or by deleting it.",
                    "zone"_attr = redact(tag),
                    "namespace"_attr = distribution.nss());
            }
            continue;
        }

        // Calculate the rounded optimal number of chunks per shard
        const size_t idealNumberOfChunksPerShardForTag =
            (size_t)std::roundf(totalNumberOfChunksWithTag / (float)totalNumberOfShardsWithTag);

        while (_singleZoneBalance(shardStats,
                                  distribution,
                                  tag,
                                  idealNumberOfChunksPerShardForTag,
                                  &migrations,
                                  availableShards,
                                  forceJumbo ? MoveChunkRequest::ForceJumbo::kForceBalancer
                                             : MoveChunkRequest::ForceJumbo::kDoNotForce))
            ;
    }

    return migrations;
}

boost::optional<MigrateInfo> BalancerPolicy::balanceSingleChunk(
    const ChunkType& chunk,
    const ShardStatisticsVector& shardStats,
    const DistributionStatus& distribution) {
    const string tag = distribution.getTagForRange(chunk.getRange());

    stdx::unordered_set<ShardId> availableShards;
    std::transform(shardStats.begin(),
                   shardStats.end(),
                   std::inserter(availableShards, availableShards.end()),
                   [](const ClusterStatistics::ShardStatistics& shardStatistics) -> ShardId {
                       return shardStatistics.shardId;
                   });

    ShardId newShardId =
        _getLeastLoadedReceiverShard(shardStats, distribution, tag, availableShards);
    if (!newShardId.isValid() || newShardId == chunk.getShard()) {
        return boost::optional<MigrateInfo>();
    }

    return MigrateInfo(
        newShardId, chunk, MoveChunkRequest::ForceJumbo::kDoNotForce, MigrateInfo::chunksImbalance);
}

bool BalancerPolicy::_singleZoneBalance(const ShardStatisticsVector& shardStats,
                                        const DistributionStatus& distribution,
                                        const string& tag,
                                        size_t idealNumberOfChunksPerShardForTag,
                                        vector<MigrateInfo>* migrations,
                                        stdx::unordered_set<ShardId>* availableShards,
                                        MoveChunkRequest::ForceJumbo forceJumbo) {
    const ShardId from = _getMostOverloadedShard(shardStats, distribution, tag, *availableShards);
    if (!from.isValid())
        return false;

    const size_t max = distribution.numberOfChunksInShardWithTag(from, tag);

    // Do not use a shard if it already has less entries than the optimal per-shard chunk count
    if (max <= idealNumberOfChunksPerShardForTag)
        return false;

    const ShardId to =
        _getLeastLoadedReceiverShard(shardStats, distribution, tag, *availableShards);
    if (!to.isValid()) {
        if (migrations->empty()) {
            LOGV2(21882,
                  "No available shards to take chunks for zone {zone}",
                  "No available shards to take chunks for zone",
                  "zone"_attr = tag);
        }
        return false;
    }

    const size_t min = distribution.numberOfChunksInShardWithTag(to, tag);

    // Do not use a shard if it already has more entries than the optimal per-shard chunk count
    if (min >= idealNumberOfChunksPerShardForTag)
        return false;

    const size_t imbalance = max - idealNumberOfChunksPerShardForTag;

    LOGV2_DEBUG(
        21883,
        1,
        "collection: {namespace}, zone: {zone}, donor: {fromShardId} chunks on "
        " {fromShardChunkCount}, receiver: {toShardId} chunks on {toShardChunkCount}, "
        "ideal: {idealNumberOfChunksPerShardForTag}, threshold: {chunkCountImbalanceThreshold}",
        "Balancing single zone",
        "namespace"_attr = distribution.nss().ns(),
        "zone"_attr = tag,
        "fromShardId"_attr = from,
        "fromShardChunkCount"_attr = max,
        "toShardId"_attr = to,
        "toShardChunkCount"_attr = min,
        "idealNumberOfChunksPerShardForTag"_attr = idealNumberOfChunksPerShardForTag,
        "chunkCountImbalanceThreshold"_attr = kDefaultImbalanceThreshold);

    // Check whether it is necessary to balance within this zone
    if (imbalance < kDefaultImbalanceThreshold)
        return false;

    const auto& fromShardId = from;
    const auto& toShardId = to;

    unsigned numJumboChunks = 0;

    const auto chunkFound =
        !distribution.forEachChunkOnShardInZone(fromShardId, tag, [&](const auto& chunk) {
            if (chunk.isJumbo()) {
                numJumboChunks++;
                return true;  // continue
            }

            migrations->emplace_back(toShardId,
                                     makeChunkType(distribution.nss(), chunk),
                                     forceJumbo,
                                     MigrateInfo::chunksImbalance);
            invariant(availableShards->erase(chunk.getShardId()));
            invariant(availableShards->erase(toShardId));
            return false;  // break
        });

    invariant(chunkFound || numJumboChunks,
              str::stream() << "Expected to find at least one chunk for shard '"
                            << fromShardId.toString() << "' in zone '" << tag << "'");

    if (!chunkFound && numJumboChunks) {
        LOGV2_WARNING(
            21894,
            "Shard: {shardId}, collection: {namespace} has only jumbo chunks for "
            "zone \'{zone}\' and cannot be balanced. Jumbo chunks count: {numJumboChunks}",
            "Shard has only jumbo chunks for and cannot be balanced",
            "shardId"_attr = from,
            "namespace"_attr = distribution.nss().ns(),
            "zone"_attr = tag,
            "numJumboChunks"_attr = numJumboChunks);
    }

    return chunkFound;
}

ZoneRange::ZoneRange(const BSONObj& a_min, const BSONObj& a_max, const std::string& _zone)
    : min(a_min.getOwned()), max(a_max.getOwned()), zone(_zone) {}

string ZoneRange::toString() const {
    return str::stream() << min << " -->> " << max << "  on  " << zone;
}

MigrateInfo::MigrateInfo(const ShardId& a_to,
                         const ChunkType& a_chunk,
                         const MoveChunkRequest::ForceJumbo a_forceJumbo,
                         MigrationReason a_reason) {
    invariant(a_chunk.validate());
    invariant(a_to.isValid());

    to = a_to;

    nss = a_chunk.getNS();
    from = a_chunk.getShard();
    minKey = a_chunk.getMin();
    maxKey = a_chunk.getMax();
    version = a_chunk.getVersion();
    forceJumbo = a_forceJumbo;
    reason = a_reason;
}

std::string MigrateInfo::getName() const {
    // Generates a unique name for a MigrateInfo based on the namespace and the lower bound
    // of the chunk being moved.
    StringBuilder buf;
    buf << nss.ns() << "-";

    BSONObjIterator i(minKey);
    while (i.more()) {
        BSONElement e = i.next();
        buf << e.fieldName() << "_" << e.toString(false, true);
    }

    return buf.str();
}

BSONObj MigrateInfo::getMigrationTypeQuery() const {
    // Generates a query object for a single MigrationType based on the namespace and the
    // lower bound of the chunk being moved.
    return BSON(MigrationType::ns(nss.ns()) << MigrationType::min(minKey));
}

string MigrateInfo::toString() const {
    return str::stream() << nss.ns() << ": [" << minKey << ", " << maxKey << "), from " << from
                         << ", to " << to;
}

}  // namespace mongo
