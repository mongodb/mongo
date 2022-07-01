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

#include <set>
#include <variant>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/balancer/cluster_statistics.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/request_types/auto_split_vector_gen.h"
#include "mongo/s/request_types/move_range_request_gen.h"
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
    MigrateInfo(const ShardId& a_to,
                const NamespaceString& a_nss,
                const ChunkType& a_chunk,
                ForceJumbo a_forceJumbo);

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

    BSONObj getMigrationTypeQuery() const;

    std::string toString() const;

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
    // TODO SERVER-65322 make `optMaxChunkSizeBytes` non-optional
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
              const ChunkVersion& collectionVersion,
              const ChunkVersion& chunkVersion,
              const BSONObj& minKey,
              const BSONObj& maxKey,
              SplitPoints splitKeys);

    std::string toString() const;

    ShardId shardId;
    NamespaceString nss;
    ChunkVersion collectionVersion;
    ChunkVersion chunkVersion;
    BSONObj minKey;
    BSONObj maxKey;
    SplitPoints splitKeys;
};

typedef std::vector<SplitInfo> SplitInfoVector;

struct SplitInfoWithKeyPattern {
    SplitInfoWithKeyPattern(const ShardId& shardId,
                            const NamespaceString& nss,
                            const ChunkVersion& collectionVersion,
                            const BSONObj& minKey,
                            const BSONObj& maxKey,
                            SplitPoints splitKeys,
                            const UUID& uuid,
                            const BSONObj& keyPattern);
    SplitInfo info;
    UUID uuid;
    BSONObj keyPattern;
};

struct AutoSplitVectorInfo {
    AutoSplitVectorInfo(const ShardId& shardId,
                        const NamespaceString& nss,
                        const UUID& uuid,
                        const ChunkVersion& collectionVersion,
                        const BSONObj& keyPattern,
                        const BSONObj& minKey,
                        const BSONObj& maxKey,
                        long long maxChunkSizeBytes);

    ShardId shardId;
    NamespaceString nss;
    UUID uuid;
    ChunkVersion collectionVersion;
    BSONObj keyPattern;
    BSONObj minKey;
    BSONObj maxKey;
    long long maxChunkSizeBytes;
};

struct MergeInfo {
    MergeInfo(const ShardId& shardId,
              const NamespaceString& nss,
              const UUID& uuid,
              const ChunkVersion& collectionVersion,
              const ChunkRange& chunkRange);

    std::string toString() const;

    ShardId shardId;
    NamespaceString nss;
    UUID uuid;
    ChunkVersion collectionVersion;
    ChunkRange chunkRange;
};

struct DataSizeInfo {
    DataSizeInfo(const ShardId& shardId,
                 const NamespaceString& nss,
                 const UUID& uuid,
                 const ChunkRange& chunkRange,
                 const ChunkVersion& version,
                 const KeyPattern& keyPattern,
                 bool estimatedValue);

    ShardId shardId;
    NamespaceString nss;
    UUID uuid;
    ChunkRange chunkRange;
    ChunkVersion version;
    KeyPattern keyPattern;
    bool estimatedValue;
};

struct DataSizeResponse {
    DataSizeResponse(long long sizeBytes, long long numObjects)
        : sizeBytes(sizeBytes), numObjects(numObjects) {}

    long long sizeBytes;
    long long numObjects;
};

typedef stdx::
    variant<MergeInfo, AutoSplitVectorInfo, DataSizeInfo, SplitInfoWithKeyPattern, MigrateInfo>
        DefragmentationAction;

typedef stdx::variant<Status, StatusWith<AutoSplitVectorResponse>, StatusWith<DataSizeResponse>>
    DefragmentationActionResponse;

typedef std::vector<ClusterStatistics::ShardStatistics> ShardStatisticsVector;
typedef std::map<ShardId, std::vector<ChunkType>> ShardToChunksMap;

/*
 * Keeps track of info needed for data size aware balancing.
 */
struct CollectionDataSizeInfoForBalancing {
    CollectionDataSizeInfoForBalancing(std::map<ShardId, int64_t>&& shardToDataSizeMap,
                                       long maxChunkSizeBytes)
        : shardToDataSizeMap(std::move(shardToDataSizeMap)), maxChunkSizeBytes(maxChunkSizeBytes) {}

    std::map<ShardId, int64_t> shardToDataSizeMap;
    const int64_t maxChunkSizeBytes;
};

/**
 * Keeps track of zones for a collection.
 */
class ZoneInfo {
public:
    ZoneInfo();

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

    /**
     * read all tags for collection via the catalog client and add to the zoneInfo
     */
    static Status addTagsFromCatalog(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const KeyPattern& keyPattern,
                                     ZoneInfo& zoneInfo);

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
    DistributionStatus(NamespaceString nss, ShardToChunksMap shardToChunksMap);
    DistributionStatus(DistributionStatus&&) = default;
    ~DistributionStatus() {}

    /**
     * Returns the namespace for which this balance status applies.
     */
    const NamespaceString& nss() const {
        return _nss;
    }

    /**
     * Appends the specified range to the set of ranges tracked for this collection and checks if
     * it overlaps with existing ranges.
     */
    Status addRangeToZone(const ZoneRange& range);

    /**
     * Returns total number of chunks across all shards.
     */
    size_t totalChunks() const;

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
     * Returns all chunks for the specified shard.
     */
    const std::vector<ChunkType>& getChunks(const ShardId& shardId) const;

    /**
     * Returns all tag ranges defined for the collection.
     */
    const BSONObjIndexedMap<ZoneRange>& tagRanges() const {
        return _zoneInfo.zoneRanges();
    }

    /**
     * Returns all tags defined for the collection.
     */
    const std::set<std::string>& tags() const {
        return _zoneInfo.allZones();
    }

    /**
     * Direct access to zone info
     */
    ZoneInfo& zoneInfo() {
        return _zoneInfo;
    }

    /**
     * Using the set of tags defined for the collection, returns what tag corresponds to the
     * specified chunk. If the chunk doesn't fall into any tag returns the empty string.
     */
    std::string getTagForChunk(const ChunkType& chunk) const;

    /**
     * Returns a BSON/string representation of this distribution status.
     */
    void report(BSONObjBuilder* builder) const;
    std::string toString() const;

private:
    // Namespace for which this distribution applies
    NamespaceString _nss;

    // Map of what chunks are owned by each shard
    ShardToChunksMap _shardChunks;

    // Info for zones.
    ZoneInfo _zoneInfo;
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
     * The usedShards parameter is in/out and it contains the set of shards, which have already been
     * used for migrations. Used so we don't return multiple conflicting migrations for the same
     * shard.
     */
    static MigrateInfosWithReason balance(
        const ShardStatisticsVector& shardStats,
        const DistributionStatus& distribution,
        const boost::optional<CollectionDataSizeInfoForBalancing>& collDataSizeInfo,
        stdx::unordered_set<ShardId>* usedShards,
        bool forceJumbo);

    /**
     * Using the specified distribution information, returns a suggested better location for the
     * specified chunk if one is available.
     */
    static boost::optional<MigrateInfo> balanceSingleChunk(const ChunkType& chunk,
                                                           const ShardStatisticsVector& shardStats,
                                                           const DistributionStatus& distribution);

private:
    /*
     * Only considers shards with the specified tag, all shards in case the tag is empty.
     *
     * Returns a tuple <ShardID, number of chunks> referring the shard with less chunks.
     *
     * If balancing based on collection size on shards:
     *  - Returns a tuple <ShardID, amount of data in bytes> referring the shard with less data.
     */
    static std::tuple<ShardId, int64_t> _getLeastLoadedReceiverShard(
        const ShardStatisticsVector& shardStats,
        const DistributionStatus& distribution,
        const boost::optional<CollectionDataSizeInfoForBalancing>& collDataSizeInfo,
        const std::string& tag,
        const stdx::unordered_set<ShardId>& excludedShards);

    /**
     * Only considers shards with the specified tag, all shards in case the tag is empty.
     *
     * If balancing based on number of chunks:
     *  - Returns a tuple <ShardID, number of chunks> referring the shard with more chunks.
     *
     * If balancing based on collection size on shards:
     *  - Returns a tuple <ShardID, amount of data in bytes> referring the shard with more data.
     */
    static std::tuple<ShardId, int64_t> _getMostOverloadedShard(
        const ShardStatisticsVector& shardStats,
        const DistributionStatus& distribution,
        const boost::optional<CollectionDataSizeInfoForBalancing>& collDataSizeInfo,
        const std::string& chunkTag,
        const stdx::unordered_set<ShardId>& excludedShards);

    /**
     * Selects one chunk for the specified zone (if appropriate) to be moved in order to bring the
     * deviation of the shards chunk contents closer to even across all shards in the specified
     * zone. Takes into account and updates the shards, which have already been used for migrations.
     *
     * Returns true if a migration was suggested, false otherwise. This method is intented to be
     * called multiple times until all posible migrations for a zone have been selected.
     */
    static bool _singleZoneBalanceBasedOnChunks(const ShardStatisticsVector& shardStats,
                                                const DistributionStatus& distribution,
                                                const std::string& tag,
                                                size_t totalNumberOfShardsWithTag,
                                                std::vector<MigrateInfo>* migrations,
                                                stdx::unordered_set<ShardId>* usedShards,
                                                ForceJumbo forceJumbo);

    /**
     * Selects one range for the specified zone (if appropriate) to be moved in order to bring the
     * deviation of the collection data size closer to even across all shards in the specified
     * zone. Takes into account and updates the shards, which have already been used for migrations.
     *
     * Returns true if a migration was suggested, false otherwise. This method is intented to be
     * called multiple times until all posible migrations for a zone have been selected.
     */
    static bool _singleZoneBalanceBasedOnDataSize(
        const ShardStatisticsVector& shardStats,
        const DistributionStatus& distribution,
        const CollectionDataSizeInfoForBalancing& collDataSizeInfo,
        const std::string& tag,
        std::vector<MigrateInfo>* migrations,
        stdx::unordered_set<ShardId>* usedShards,
        ForceJumbo forceJumbo);
};

}  // namespace mongo
