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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/balancer/cluster_statistics.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/s/request_types/get_stats_for_balancing_gen.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {

struct ZoneRange {
    ZoneRange(const BSONObj& a_min, const BSONObj& a_max, const std::string& _zone);

    std::string toString() const;

    BSONObj min;
    BSONObj max;
    std::string zone;
};

struct MigrateInfo {
    MigrateInfo(const ShardId& a_to,
                const NamespaceString& a_nss,
                const ChunkType& a_chunk,
                ForceJumbo a_forceJumbo,
                boost::optional<int64_t> maxChunkSizeBytes = boost::none);

    MigrateInfo(const ShardId& a_to,
                const ShardId& a_from,
                const NamespaceString& a_nss,
                const UUID& a_uuid,
                const BSONObj& a_min,
                const boost::optional<BSONObj>& a_max,
                const ChunkVersion& a_version,
                ForceJumbo a_forceJumbo,
                boost::optional<int64_t> maxChunkSizeBytes = boost::none);

    std::string getName() const;

    std::string toString() const;

    boost::optional<int64_t> getMaxChunkSizeBytes() const;

    NamespaceString nss;
    UUID uuid;
    ShardId to;
    ShardId from;
    BSONObj minKey;

    // May be optional in case of moveRange
    boost::optional<BSONObj> maxKey;
    ChunkVersion version;
    ForceJumbo forceJumbo;

    // Set only in case of data-size aware balancing
    boost::optional<int64_t> optMaxChunkSizeBytes;
};

enum MigrationReason { none, drain, zoneViolation, chunksImbalance };

typedef std::vector<MigrateInfo> MigrateInfoVector;

typedef std::pair<MigrateInfoVector, MigrationReason> MigrateInfosWithReason;

typedef std::vector<BSONObj> SplitPoints;

/**
 * Describes a chunk which needs to be split, because it violates the balancer policy.
 */
struct SplitInfo {
    SplitInfo(const ShardId& shardId,
              const NamespaceString& nss,
              const ChunkVersion& collectionPlacementVersion,
              const ChunkVersion& chunkVersion,
              const BSONObj& minKey,
              const BSONObj& maxKey,
              SplitPoints splitKeys);

    std::string toString() const;

    ShardId shardId;
    NamespaceString nss;
    ChunkVersion collectionPlacementVersion;
    ChunkVersion chunkVersion;
    BSONObj minKey;
    BSONObj maxKey;
    SplitPoints splitKeys;
};

typedef std::vector<SplitInfo> SplitInfoVector;

struct MergeInfo {
    MergeInfo(const ShardId& shardId,
              const NamespaceString& nss,
              const UUID& uuid,
              const ChunkVersion& collectionPlacementVersion,
              const ChunkRange& chunkRange);

    std::string toString() const;

    ShardId shardId;
    NamespaceString nss;
    UUID uuid;
    ChunkVersion collectionPlacementVersion;
    ChunkRange chunkRange;
};

struct MergeAllChunksOnShardInfo {
    MergeAllChunksOnShardInfo(const ShardId& shardId, const NamespaceString& nss);

    std::string toString() const;

    ShardId shardId;
    NamespaceString nss;

    bool applyThrottling{false};
};

struct DataSizeInfo {
    DataSizeInfo(const ShardId& shardId,
                 const NamespaceString& nss,
                 const UUID& uuid,
                 const ChunkRange& chunkRange,
                 const ShardVersion& version,
                 const KeyPattern& keyPattern,
                 bool estimatedValue,
                 int64_t maxSize);

    ShardId shardId;
    NamespaceString nss;
    UUID uuid;
    ChunkRange chunkRange;
    // Use ShardVersion for CRUD targeting since datasize is considered a CRUD operation, not a DDL
    // operation.
    ShardVersion version;
    KeyPattern keyPattern;
    bool estimatedValue;
    int64_t maxSize;
};

struct DataSizeResponse {
    DataSizeResponse(long long sizeBytes, long long numObjects, bool maxSizeReached)
        : sizeBytes(sizeBytes), numObjects(numObjects), maxSizeReached(maxSizeReached) {}

    long long sizeBytes;
    long long numObjects;
    bool maxSizeReached;
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

typedef int NumMergedChunks;

typedef std::variant<MergeInfo, DataSizeInfo, MigrateInfo, MergeAllChunksOnShardInfo>
    BalancerStreamAction;

typedef std::variant<Status, StatusWith<DataSizeResponse>, StatusWith<NumMergedChunks>>
    BalancerStreamActionResponse;

typedef std::vector<ClusterStatistics::ShardStatistics> ShardStatisticsVector;
typedef StringMap<StringMap<ShardZoneInfo>> ShardZoneInfoMap;

using ShardDataSizeMap = std::map<ShardId, int64_t>;
using NamespaceStringToShardDataSizeMap = stdx::unordered_map<NamespaceString, ShardDataSizeMap>;
/*
 * Keeps track of info needed for data size aware balancing.
 */
struct CollectionDataSizeInfoForBalancing {
    CollectionDataSizeInfoForBalancing(ShardDataSizeMap&& shardToDataSizeMap,
                                       long maxChunkSizeBytes)
        : shardToDataSizeMap(std::move(shardToDataSizeMap)), maxChunkSizeBytes(maxChunkSizeBytes) {}

    ShardDataSizeMap shardToDataSizeMap;
    const int64_t maxChunkSizeBytes;
};

NamespaceStringToShardDataSizeMap getStatsForBalancing(
    OperationContext* opCtx,
    const std::vector<ShardId>& shardIds,
    const std::vector<NamespaceWithOptionalUUID>& namespacesWithUUIDsForStatsRequest);

ShardDataSizeMap getStatsForBalancing(
    OperationContext* opCtx,
    const std::vector<ShardId>& shardIds,
    const NamespaceWithOptionalUUID& namespaceWithUUIDsForStatsRequest);

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
     * Using the set of zones added so far, returns what zone corresponds to the specified range.
     * Returns an empty string if the chunk doesn't fall into any zone.
     */
    std::string getZoneForRange(const ChunkRange& chunkRange) const;

    /**
     * Returns all zone ranges defined.
     */
    const BSONObjIndexedMap<ZoneRange>& zoneRanges() const {
        return _zoneRanges;
    }

    /**
     * Retrieves the collection zones from the catalog client
     */
    static StatusWith<ZoneInfo> getZonesForCollection(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      const KeyPattern& keyPattern);

private:
    // Map of zone max key to the zone description
    BSONObjIndexedMap<ZoneRange> _zoneRanges;

    // Set of all zones defined for this collection
    std::set<std::string> _allZones;
};

class ChunkManager;

/**
 * This class constitutes a cache of the chunk distribution across the entire cluster along with the
 * zone boundaries imposed on it. This information is stored in format, which makes it efficient to
 * query utilization statististics and to decide what to balance.
 */
class DistributionStatus final {
    DistributionStatus(const DistributionStatus&) = delete;
    DistributionStatus& operator=(const DistributionStatus&) = delete;

public:
    DistributionStatus(NamespaceString nss, ZoneInfo zoneInfo, const ChunkManager& chunkMngr);
    DistributionStatus(DistributionStatus&&) = default;
    ~DistributionStatus() {}

    /**
     * Returns the namespace for which this balance status applies.
     */
    const NamespaceString& nss() const {
        return _nss;
    }

    /**
     * Returns number of chunks in the specified shard.
     */
    size_t numberOfChunksInShard(const ShardId& shardId) const;

    /**
     * Returns all zones defined for the collection.
     */
    const std::set<std::string>& zones() const {
        return _zoneInfo.allZones();
    }

    const ChunkManager& getChunkManager() const {
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
                tassert(8236530,
                        fmt::format("Unexpected first normalized zone for shard '{}'. Expected "
                                    "'{}' but found '{}'",
                                    shardId.toString(),
                                    zoneName,
                                    zoneRange.zone),
                        zoneRange.zone == zoneName);
            } else if (zoneRange.zone != zoneName) {
                continue;
            }

            // For the first range in zone we have pre-cached the minKey of the first chunk,
            // thus we can start iterating from that one.
            // For the subsequent ranges in this zone we start iterating from the minKey of
            // the range itself.
            const auto firstKey = isFirstRange ? shardZoneInfo.firstChunkMinKey : zoneRange.min;

            getChunkManager().forEachOverlappingChunk(
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

    ChunkManager _chunkMngr;
};

class BalancerPolicy {
public:
    /**
     * Determines whether a shard with the specified utilization statistics would be able to accept
     * a chunk with the specified zone. According to the policy a shard cannot accept chunks if its
     * size is maxed out and if the chunk's zone conflicts with the zone of the shard.
     */
    static Status isShardSuitableReceiver(const ClusterStatistics::ShardStatistics& stat,
                                          const std::string& chunkZone);

    /**
     * Returns a suggested set of chunks or ranges to move within a collection's shards, given the
     * specified state of the shards (draining, max size reached, etc) and the number of chunks or
     * data size for that collection. If the policy doesn't recommend anything to move, it returns
     * an empty vector. The entries in the vector do are all for separate source/destination shards
     * and as such do not need to be done serially and can be scheduled in parallel.
     *
     * The balancing logic calculates the optimum number of chunks per shard for each zone and if
     * any of the shards have chunks, which are sufficiently higher than this number, suggests
     * moving chunks to shards, which are under this number.
     *
     * The availableShards parameter is in/out and it contains the set of shards, which haven't
     * been used for migrations yet. Used so we don't return multiple conflicting migrations for the
     * same shard.
     */
    static MigrateInfosWithReason balance(
        const ShardStatisticsVector& shardStats,
        const DistributionStatus& distribution,
        const CollectionDataSizeInfoForBalancing& collDataSizeInfo,
        stdx::unordered_set<ShardId>* availableShards,
        bool forceJumbo);

private:
    /*
     * Only considers shards with the specified zone, all shards in case the zone is empty.
     * Returns a tuple <ShardID, amount of data in bytes> referring the shard with less data.
     */
    static std::tuple<ShardId, int64_t> _getLeastLoadedReceiverShard(
        const ShardStatisticsVector& shardStats,
        const CollectionDataSizeInfoForBalancing& collDataSizeInfo,
        const std::string& zone,
        const stdx::unordered_set<ShardId>& availableShards);

    /**
     * Only considers shards with the specified zone, all shards in case the zone is empty.
     * Returns a tuple <ShardID, amount of data in bytes> referring the shard with more data.
     */
    static std::tuple<ShardId, int64_t> _getMostOverloadedShard(
        const ShardStatisticsVector& shardStats,
        const CollectionDataSizeInfoForBalancing& collDataSizeInfo,
        const std::string& zone,
        const stdx::unordered_set<ShardId>& availableShards);

    /**
     * Selects one range for the specified zone (if appropriate) to be moved in order to bring the
     * deviation of the collection data size closer to even across all shards in the specified
     * zone. Takes into account and updates the shards, which haven't been used for migrations yet.
     *
     * Returns true if a migration was suggested, false otherwise. This method is intented to be
     * called multiple times until all posible migrations for a zone have been selected.
     */
    static bool _singleZoneBalanceBasedOnDataSize(
        const ShardStatisticsVector& shardStats,
        const DistributionStatus& distribution,
        const CollectionDataSizeInfoForBalancing& collDataSizeInfo,
        const std::string& zone,
        int64_t idealDataSizePerShardForZone,
        std::vector<MigrateInfo>* migrations,
        stdx::unordered_set<ShardId>* availableShards,
        ForceJumbo forceJumbo);
};

}  // namespace mongo
