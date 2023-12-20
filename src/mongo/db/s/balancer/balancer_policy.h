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

#include <unordered_set>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/balancer/cluster_statistics.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/request_types/move_chunk_request.h"
#include "mongo/s/shard_id.h"

namespace mongo {

struct ZoneRange {
    ZoneRange(const BSONObj& a_min, const BSONObj& a_max, const std::string& _zone);

    std::string toString() const;

    BSONObj min;
    BSONObj max;
    std::string zone;
};

struct MigrateInfo {
    enum MigrationReason { drain, zoneViolation, chunksImbalance };

    MigrateInfo(const ShardId& a_to,
                const ChunkType& a_chunk,
                MoveChunkRequest::ForceJumbo a_forceJumbo,
                MigrationReason a_reason);

    std::string getName() const;

    BSONObj getMigrationTypeQuery() const;

    std::string toString() const;

    NamespaceString nss;
    ShardId to;
    ShardId from;
    BSONObj minKey;
    BSONObj maxKey;
    ChunkVersion version;
    MoveChunkRequest::ForceJumbo forceJumbo;
    MigrationReason reason;
};

struct ShardZoneInfo {
    ShardZoneInfo(size_t numChunks, size_t firstNormalizedZoneIdx, const BSONObj& firstChunkMinKey)
        : numChunks(numChunks),
          firstNormalizedZoneIdx(firstNormalizedZoneIdx),
          firstChunkMinKey(firstChunkMinKey) {}

    // Total number of chunks this shard has for this zone
    size_t numChunks;
    // Index in the vector of normalised zones of the first zone range that contains the first chunk
    // for this shard in this zone
    size_t firstNormalizedZoneIdx;
    // minKey of the first chunk this shard has in this zone
    BSONObj firstChunkMinKey;
};

typedef std::vector<ClusterStatistics::ShardStatistics> ShardStatisticsVector;
typedef StringMap<StringMap<ShardZoneInfo>> ShardZoneInfoMap;

/**
 * Keeps track of zones for a collection.
 */
class ZoneInfo {
public:
    static const std::string kNoZoneName;

    ZoneInfo();
    ZoneInfo(ZoneInfo&&) = default;

    /**
     * Appends the specified range to the set of ranges tracked for this collection and checks if
     * it overlaps with existing ranges.
     */
    Status addRangeToZone(const ZoneRange& range);

    /**
     * Returns all zones added so far.
     */
    const std::set<std::string>& allZones() const {
        return _allZones;
    }

    /**
     * Using the set of zones added so far, returns what zone corresponds to the specified chunk.
     * Returns an empty string if the chunk doesn't fall into any zone.
     */
    std::string getZoneForChunk(const ChunkRange& chunkRange) const;

    /**
     * Returns all zone ranges defined.
     */
    const BSONObjIndexedMap<ZoneRange>& zoneRanges() const {
        return _zoneRanges;
    }

    const ZoneRange& getZoneRange(const std::string& zoneName) const {
        for (const auto& [_, zoneRange] : _zoneRanges) {
            if (zoneRange.zone == zoneName)
                return zoneRange;
        }
        MONGO_UNREACHABLE;
    }

private:
    // Map of zone max key to the zone description
    BSONObjIndexedMap<ZoneRange> _zoneRanges;

    // Set of all zones defined for this collection
    std::set<std::string> _allZones;
};

/**
 * This class constitutes a cache of the chunk distribution across the entire cluster along with the
 * zone boundaries imposed on it. This information is stored in format, which makes it efficient to
 * query utilization statististics and to decide what to balance.
 */
class DistributionStatus {
    DistributionStatus(const DistributionStatus&) = delete;
    DistributionStatus& operator=(const DistributionStatus&) = delete;

public:
    DistributionStatus(NamespaceString nss, ZoneInfo zoneInfo, const ChunkManager* chunkMngr);
    DistributionStatus(DistributionStatus&&) = default;

    /**
     * Returns the namespace for which this balance status applies.
     */
    const NamespaceString& nss() const {
        return _nss;
    }

    /**
     * Returns the total number of chunks across all shards, which fall into the specified zone's
     * range.
     */
    size_t totalChunksWithTag(const std::string& tag) const;

    /**
     * Returns number of chunks in the specified shard.
     */
    size_t numberOfChunksInShard(const ShardId& shardId) const;

    /**
     * Returns number of chunks in the specified shard, which have the given tag.
     */
    size_t numberOfChunksInShardWithTag(const ShardId& shardId, const std::string& tag) const;

    /**
     * Returns all tags defined for the collection.
     */
    const std::set<std::string>& tags() const {
        return _zoneInfo.allZones();
    }

    /**
     * Using the set of tags defined for the collection, returns what tag corresponds to the
     * specified chunk. If the chunk doesn't fall into any tag returns the empty string.
     */
    std::string getTagForRange(const ChunkRange& range) const;

    const ChunkManager* getChunkManager() const {
        return _chunkMngr;
    }

    const ZoneInfo& getZoneInfo() const {
        return _zoneInfo;
    }

    const StringMap<ShardZoneInfo>& getZoneInfoForShard(const ShardId& shardId) const;

    /**
     * Loop through each chunk on this shard within the given zone invoking 'handler' for each one
     * of them.
     *
     * The iteration stops either when all the chunks have been visited (the method
     * will return 'true') or the first time 'handler' returns 'false' (in which case the method
     * will return 'false').
     *
     * Effectively, return of 'true' means all chunks were visited and none matched, and
     * 'false' means the hanlder return 'false' before visiting all chunks.
     */
    template <typename Callable>
    bool forEachChunkOnShardInZone(const ShardId& shardId,
                                   const std::string& zoneName,
                                   Callable&& handler) const {

        bool shouldContinue = true;

        const auto& shardZoneInfoMap = getZoneInfoForShard(shardId);
        auto shardZoneInfoIt = shardZoneInfoMap.find(zoneName);
        if (shardZoneInfoIt == shardZoneInfoMap.end()) {
            return shouldContinue;
        }
        const auto& shardZoneInfo = shardZoneInfoIt->second;

        // Start from the first normalized zone that contains chunks for this shard
        const auto initialZoneIt = _normalizedZones.cbegin() + shardZoneInfo.firstNormalizedZoneIdx;

        for (auto normalizedZoneIt = initialZoneIt; normalizedZoneIt < _normalizedZones.cend();
             normalizedZoneIt++) {
            const auto& zoneRange = *normalizedZoneIt;

            const auto isFirstRange = (normalizedZoneIt == initialZoneIt);

            if (isFirstRange) {
                invariant(zoneRange.zone == zoneName,
                          str::stream() << "Unexpected first normalized zone for shard '"
                                        << shardId.toString() << "'. Expected '" << zoneName
                                        << "' but found '" << zoneRange.zone << "'");
            } else if (zoneRange.zone != zoneName) {
                continue;
            }

            // For the first range in zone we have pre-cached the minKey of the first chunk,
            // thus we can start iterating from that one.
            // For the subsequent ranges in this zone we start iterating from the minKey of
            // the range itself.
            const auto firstKey = isFirstRange ? shardZoneInfo.firstChunkMinKey : zoneRange.min;

            getChunkManager()->forEachOverlappingChunk(
                firstKey, zoneRange.max, false /* isMaxInclusive */, [&](const auto& chunk) {
                    if (chunk.getShardId() != shardId) {
                        return true;  // continue
                    }
                    if (!handler(chunk)) {
                        shouldContinue = false;
                    };
                    return shouldContinue;
                });

            if (!shouldContinue) {
                break;
            }
        }
        return shouldContinue;
    }

    /**
     * Returns a BSON/string representation of this distribution status.
     */
    void report(BSONObjBuilder* builder) const;
    std::string toString() const;

private:
    // Namespace for which this distribution applies
    NamespaceString _nss;

    // Map that tracks how many chunks every shard is owning in each zone
    // shardId -> zoneName -> shardZoneInfo
    ShardZoneInfoMap _shardZoneInfoMap;

    // Info for zones.
    ZoneInfo _zoneInfo;

    // Normalized zone are calculated starting from the currently configured zone in `config.tags`
    // and the chunks provided by @this._chunkManager.
    //
    // The normalization process is performed to guarantee the following properties:
    //  - **All zone ranges are contiguous.** If there was a gap between two zones ranges we fill it
    //  with a range associated to the special kNoZone.
    //
    //  - **Range boundaries always align with chunk boundaries.** If a zone range covers only
    //  partially a chunk, boundaries of that zone will be shrunk so that the normalized zone won't
    //  overlap with that chunk. Boundaries of a normalized zone will never fall in the middle of a
    //  chunk.
    std::vector<ZoneRange> _normalizedZones;

    const ChunkManager* _chunkMngr;
};

class BalancerPolicy {
public:
    /**
     * Determines whether a shard with the specified utilization statistics would be able to accept
     * a chunk with the specified tag. According to the policy a shard cannot accept chunks if its
     * size is maxed out and if the chunk's tag conflicts with the tag of the shard.
     */
    static Status isShardSuitableReceiver(const ClusterStatistics::ShardStatistics& stat,
                                          const std::string& chunkTag);

    /**
     * Returns a suggested set of chunks to move whithin a collection's shards, given the specified
     * state of the shards (draining, max size reached, etc) and the number of chunks for that
     * collection. If the policy doesn't recommend anything to move, it returns an empty vector. The
     * entries in the vector do are all for separate source/destination shards and as such do not
     * need to be done serially and can be scheduled in parallel.
     *
     * The balancing logic calculates the optimum number of chunks per shard for each zone and if
     * any of the shards have chunks, which are sufficiently higher than this number, suggests
     * moving chunks to shards, which are under this number.
     *
     * The availableShards parameter is in/out and it contains the set of shards, which are still
     * available to participate on a migration. Used so we don't return multiple conflicting
     * migrations for the same shard.
     */
    static std::vector<MigrateInfo> balance(const ShardStatisticsVector& shardStats,
                                            const DistributionStatus& distribution,
                                            stdx::unordered_set<ShardId>* availableShards,
                                            bool forceJumbo);

    /**
     * Using the specified distribution information, returns a suggested better location for the
     * specified chunk if one is available.
     */
    static boost::optional<MigrateInfo> balanceSingleChunk(const ChunkType& chunk,
                                                           const ShardStatisticsVector& shardStats,
                                                           const DistributionStatus& distribution);

private:
    /**
     * Return the shard with the specified tag, which has the least number of chunks. If the tag is
     * empty, considers all shards.
     */
    static ShardId _getLeastLoadedReceiverShard(
        const ShardStatisticsVector& shardStats,
        const DistributionStatus& distribution,
        const std::string& tag,
        const stdx::unordered_set<ShardId>& availableShards);

    /**
     * Return the shard which has the least number of chunks with the specified tag. If the tag is
     * empty, considers all chunks.
     */
    static ShardId _getMostOverloadedShard(const ShardStatisticsVector& shardStats,
                                           const DistributionStatus& distribution,
                                           const std::string& chunkTag,
                                           const stdx::unordered_set<ShardId>& availableShards);

    /**
     * Selects one chunk for the specified zone (if appropriate) to be moved in order to bring the
     * deviation of the shards chunk contents closer to even across all shards in the specified
     * zone. Takes into account and updates the shards, which have already been used for migrations.
     *
     * The 'idealNumberOfChunksPerShardForTag' indicates what is the ideal number of chunks which
     * each shard must have and is used to determine the imbalance and also to prevent chunks from
     * moving when not necessary.
     *
     * Returns true if a migration was suggested, false otherwise. This method is intented to be
     * called multiple times until all posible migrations for a zone have been selected.
     */
    static bool _singleZoneBalance(const ShardStatisticsVector& shardStats,
                                   const DistributionStatus& distribution,
                                   const std::string& tag,
                                   size_t idealNumberOfChunksPerShardForTag,
                                   std::vector<MigrateInfo>* migrations,
                                   stdx::unordered_set<ShardId>* availableShards,
                                   MoveChunkRequest::ForceJumbo forceJumbo);
};

}  // namespace mongo
