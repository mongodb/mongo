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

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/shard_id.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/uuid.h"

namespace mongo {

class ChunkManager;

struct PlacementVersionTargetingInfo {
    /**
     * Constructs a placement information for a collection with the specified generation, starting
     * at placementVersion {0, 0} and maxValidAfter of Timestamp{0, 0}. The expectation is that the
     * incremental refresh algorithm will increment these values as it processes the incoming
     * chunks.
     */
    explicit PlacementVersionTargetingInfo(const CollectionGeneration& generation);
    PlacementVersionTargetingInfo(ChunkVersion placementVersion, Timestamp validAfter)
        : placementVersion(std::move(placementVersion)), validAfter(std::move(validAfter)) {}

    // Max chunk version for the shard, effectively this is the shard placement version.
    ChunkVersion placementVersion;
    // Max validAfter for the shard, effectively this is the timestamp of the latest placement
    // change that occurred on a particular shard.
    Timestamp validAfter;
};

// Map from a shard to a struct indicating both the max chunk version on that shard and whether the
// shard is currently marked as needing a catalog cache refresh (stale).
using ShardPlacementVersionMap =
    stdx::unordered_map<ShardId, PlacementVersionTargetingInfo, ShardId::Hasher>;

/**
 * This class serves as a Facade around how the mapping of ranges to chunks is represented. It also
 * provides a simpler, high-level interface for domain specific operations without exposing the
 * underlying implementation.
 */
class ChunkMap {
public:
    // Vector of chunks ordered by max key in ascending order.
    using ChunkVector = std::vector<std::shared_ptr<ChunkInfo>>;
    using ChunkVectorMap = std::map<std::string, std::shared_ptr<ChunkVector>>;

    explicit ChunkMap(OID epoch, const Timestamp& timestamp, size_t chunkVectorSize)
        : _collectionPlacementVersion({epoch, timestamp}, {0, 0}),
          _maxChunkVectorSize(chunkVectorSize) {}

    size_t size() const;

    // Max version across all chunks
    ChunkVersion getVersion() const {
        return _collectionPlacementVersion;
    }

    size_t getMaxChunkVectorSize() const {
        return _maxChunkVectorSize;
    }

    const ShardPlacementVersionMap& getShardPlacementVersionMap() const {
        return _placementVersions;
    }

    const ChunkVectorMap& getChunkVectorMap() const {
        return _chunkVectorMap;
    }


    /*
     * Invoke the given handler for each std::shared_ptr<ChunkInfo> contained in this chunk map
     * until either all matching chunks have been processed or @handler returns false.
     *
     * Chunks are yielded in ascending order of shardkey (e.g. minKey to maxKey);
     *
     * When shardKey is provided the function will start yileding from the chunk that contains the
     * given shard key.
     */
    template <typename Callable>
    void forEach(Callable&& handler, const BSONObj& shardKey = BSONObj()) const {
        if (shardKey.isEmpty()) {
            for (const auto& mapIt : _chunkVectorMap) {
                for (const auto& chunkInfoPtr : *(mapIt.second)) {
                    if (!handler(chunkInfoPtr))
                        return;
                }
            }

            return;
        }

        auto shardKeyString = ShardKeyPattern::toKeyString(shardKey);

        const auto mapItBegin = _chunkVectorMap.upper_bound(shardKeyString);
        for (auto mapIt = mapItBegin; mapIt != _chunkVectorMap.end(); mapIt++) {
            const auto& chunkVector = *(mapIt->second);
            auto it = mapIt == mapItBegin ? _findIntersectingChunkIterator(shardKeyString,
                                                                           chunkVector.begin(),
                                                                           chunkVector.end(),
                                                                           true /*isMaxInclusive*/)
                                          : chunkVector.begin();
            for (; it != chunkVector.end(); ++it) {
                if (!handler(*it))
                    return;
            }
        }
    }


    /*
     * Invoke the given @handler for each std::shared_ptr<ChunkInfo> that overlaps with range [@min,
     * @max] until either all matching chunks have been processed or @handler returns false.
     *
     * Chunks are yielded in ascending order of shardkey (e.g. minKey to maxKey);
     *
     * When @isMaxInclusive is true also the chunk whose minKey is equal to @max will be yielded.
     */
    template <typename Callable>
    void forEachOverlappingChunk(const BSONObj& min,
                                 const BSONObj& max,
                                 bool isMaxInclusive,
                                 Callable&& handler) const {
        const auto minShardKeyStr = ShardKeyPattern::toKeyString(min);
        const auto maxShardKeyStr = ShardKeyPattern::toKeyString(max);
        const auto bounds =
            _overlappingVectorSlotBounds(minShardKeyStr, maxShardKeyStr, isMaxInclusive);
        for (auto mapIt = bounds.first; mapIt != bounds.second; ++mapIt) {

            const auto& chunkVector = *(mapIt->second);

            const auto chunkItBegin = [&] {
                if (mapIt == bounds.first) {
                    // On first vector we need to start from chunk that contain the given minKey
                    return _findIntersectingChunkIterator(minShardKeyStr,
                                                          chunkVector.begin(),
                                                          chunkVector.end(),
                                                          true /* isMaxInclusive */);
                }
                return chunkVector.begin();
            }();

            const auto chunkItEnd = [&] {
                if (mapIt == std::prev(bounds.second)) {
                    // On last vector we need to skip all chunks that are greater than the give
                    // maxKey
                    auto it = _findIntersectingChunkIterator(
                        maxShardKeyStr, chunkItBegin, chunkVector.end(), isMaxInclusive);
                    return it == chunkVector.end() ? it : ++it;
                }
                return chunkVector.end();
            }();

            for (auto chunkIt = chunkItBegin; chunkIt != chunkItEnd; ++chunkIt) {
                if (!handler(*chunkIt))
                    return;
            }
        }
    }

    std::shared_ptr<ChunkInfo> findIntersectingChunk(const BSONObj& shardKey) const;

    ChunkMap createMerged(ChunkVector changedChunks) const;

    BSONObj toBSON() const;

    std::string toString() const;

    static bool allElementsAreOfType(BSONType type, const BSONObj& obj);

private:
    ChunkVector::const_iterator _findIntersectingChunkIterator(const std::string& shardKeyString,
                                                               ChunkVector::const_iterator first,
                                                               ChunkVector::const_iterator last,
                                                               bool isMaxInclusive) const;

    std::pair<ChunkVectorMap::const_iterator, ChunkVectorMap::const_iterator>
    _overlappingVectorSlotBounds(const std::string& minShardKeyStr,
                                 const std::string& maxShardKeyStr,
                                 bool isMaxInclusive) const;
    ChunkMap _makeUpdated(ChunkVector&& changedChunks) const;

    void _updateShardVersionFromDiscardedChunk(const ChunkInfo& chunk);
    void _updateShardVersionFromUpdateChunk(const ChunkInfo& chunk,
                                            const ShardPlacementVersionMap& oldPlacmentVersions);
    void _commitUpdatedChunkVector(std::shared_ptr<ChunkVector>&& chunkVectorPtr,
                                   bool checkMaxKeyConsistency);
    void _mergeAndCommitUpdatedChunkVector(ChunkVectorMap::const_iterator pos,
                                           std::shared_ptr<ChunkVector>&& chunkVectorPtr);
    void _splitAndCommitUpdatedChunkVector(ChunkVectorMap::const_iterator pos,
                                           std::shared_ptr<ChunkVector>&& chunkVectorPtr);

    ChunkVectorMap _chunkVectorMap;

    // Max version across all chunks
    ChunkVersion _collectionPlacementVersion;

    // The representation of shard versions and staleness indicators for this namespace. If a
    // shard does not exist, it will not have an entry in the map.
    // Note: this declaration must not be moved before _chunkMap since it is initialized by using
    // the _chunkVectorMap instance.
    ShardPlacementVersionMap _placementVersions;

    // Maximum size of chunk vectors stored in the chunk vector map.
    // Bigger vectors will imply slower incremental refreshes (more chunks to copy) but
    // faster map copy (less chunk vector pointers to copy).
    size_t _maxChunkVectorSize;
};

/**
 * In-memory representation of the routing table for a single sharded collection at various points
 * in time.
 */
class RoutingTableHistory {
    RoutingTableHistory(const RoutingTableHistory&) = delete;
    RoutingTableHistory& operator=(const RoutingTableHistory&) = delete;

public:
    RoutingTableHistory(RoutingTableHistory&&) = default;
    RoutingTableHistory& operator=(RoutingTableHistory&&) = default;

    /**
     * Makes an instance with a routing table for collection "nss", sharded on
     * "shardKeyPattern".
     *
     * "defaultCollator" is the default collation for the collection, "unique" indicates whether
     * or not the shard key for each document will be globally unique, and "epoch" is the globally
     * unique identifier for this version of the collection.
     *
     * The "chunks" vector must contain the chunk routing information sorted in ascending order by
     * chunk version, and adhere to the requirements of the routing table update algorithm.
     *
     * The existence of "reshardingFields" inside the optional implies that this field was present
     * inside the config.collections entry when refreshing.
     */
    static RoutingTableHistory makeNew(
        NamespaceString nss,
        UUID uuid,
        KeyPattern shardKeyPattern,
        bool unsplittable,
        std::unique_ptr<CollatorInterface> defaultCollator,
        bool unique,
        OID epoch,
        const Timestamp& timestamp,
        boost::optional<TypeCollectionTimeseriesFields> timeseriesFields,
        boost::optional<TypeCollectionReshardingFields> reshardingFields,
        bool allowMigrations,
        const std::vector<ChunkType>& chunks);

    /**
     * Constructs a new instance with a routing table updated according to the changes described
     * in "changedChunks".
     *
     * The changes in "changedChunks" must be sorted in ascending order by chunk version, and adhere
     * to the requirements of the routing table update algorithm.
     *
     * The existence of timeseriesFields/reshardingFields inside the optional implies that this
     * field was present inside the config.collections entry when refreshing. An uninitialized
     * timeseriesFields/reshardingFields parameter implies that the field was not present, and will
     * clear any currently held timeseries/resharding fields inside the resulting
     * RoutingTableHistory.
     */
    RoutingTableHistory makeUpdated(
        boost::optional<TypeCollectionTimeseriesFields> timeseriesFields,
        boost::optional<TypeCollectionReshardingFields> reshardingFields,
        bool allowMigrations,
        bool unsplittable,
        const std::vector<ChunkType>& changedChunks) const;

    const NamespaceString& nss() const {
        return _nss;
    }

    const ShardKeyPattern& getShardKeyPattern() const {
        return _shardKeyPattern;
    }

    const CollatorInterface* getDefaultCollator() const {
        return _defaultCollator.get();
    }

    bool isUnique() const {
        return _unique;
    }

    /**
     * Returns the maximum version across all shards (also known as the "collection placement
     * version").
     */
    ChunkVersion getVersion() const {
        return _chunkMap.getVersion();
    }

    /**
     * Retrieves the placement version for the given shard.
     */
    ChunkVersion getVersion(const ShardId& shardId) const {
        return _getVersion(shardId).placementVersion;
    }

    /**
     * Retrieves the placement version for the given shard. Will not throw if the shard is marked as
     * stale. Only use when logging the given chunk version -- if the caller must execute logic
     * based on the returned version, use getVersion() instead.
     */
    ChunkVersion getVersionForLogging(const ShardId& shardId) const {
        return _getVersion(shardId).placementVersion;
    }

    /**
     * Retrieves the maximum validAfter timestamp for the given shard.
     */
    Timestamp getMaxValidAfter(const ShardId& shardId) const {
        return _getVersion(shardId).validAfter;
    }

    size_t numChunks() const {
        return _chunkMap.size();
    }

    template <typename Callable>
    void forEachChunk(Callable&& handler, const BSONObj& shardKey = BSONObj()) const {
        _chunkMap.forEach(std::forward<Callable>(handler), shardKey);
    }

    template <typename Callable>
    void forEachOverlappingChunk(const BSONObj& min,
                                 const BSONObj& max,
                                 bool isMaxInclusive,
                                 Callable&& handler) const {
        _chunkMap.forEachOverlappingChunk(
            min, max, isMaxInclusive, std::forward<Callable>(handler));
    }

    std::shared_ptr<ChunkInfo> findIntersectingChunk(const BSONObj& shardKey) const {
        return _chunkMap.findIntersectingChunk(shardKey);
    }

    /**
     * Returns the ids of all shards on which the collection has any chunks.
     */
    void getAllShardIds(std::set<ShardId>* all) const;

    /**
     * Returns all chunk ranges for the collection.
     */
    void getAllChunkRanges(std::set<ChunkRange>* all) const;

    /**
     * Returns the number of shards on which the collection has any chunks
     */
    size_t getNShardsOwningChunks() const {
        return _chunkMap.getShardPlacementVersionMap().size();
    }

    std::string toString() const;

    bool uuidMatches(const UUID& uuid) const {
        return _uuid == uuid;
    }

    const UUID& getUUID() const {
        return _uuid;
    }

    const boost::optional<TypeCollectionTimeseriesFields>& getTimeseriesFields() const {
        return _timeseriesFields;
    }

    const boost::optional<TypeCollectionReshardingFields>& getReshardingFields() const {
        return _reshardingFields;
    }

    bool allowMigrations() const {
        return _allowMigrations;
    }

private:
    friend class ChunkManager;

    RoutingTableHistory(NamespaceString nss,
                        UUID uuid,
                        KeyPattern shardKeyPattern,
                        bool unsplittable,
                        std::unique_ptr<CollatorInterface> defaultCollator,
                        bool unique,
                        boost::optional<TypeCollectionTimeseriesFields> timeseriesFields,
                        boost::optional<TypeCollectionReshardingFields> reshardingFields,
                        bool allowMigrations,
                        ChunkMap chunkMap);

    PlacementVersionTargetingInfo _getVersion(const ShardId& shardId) const;

    // Namespace to which this routing information corresponds
    NamespaceString _nss;

    // The UUID of the collection
    UUID _uuid;

    // The key pattern used to shard the collection
    ShardKeyPattern _shardKeyPattern;

    // True for tracked unsharded collections
    bool _unsplittable;

    // Default collation to use for routing data queries for this collection
    std::unique_ptr<CollatorInterface> _defaultCollator;

    // Whether the sharding key is unique
    bool _unique;

    // This information will be valid if the collection is a time-series buckets collection.
    boost::optional<TypeCollectionTimeseriesFields> _timeseriesFields;

    // The set of fields related to an ongoing resharding operation involving this collection. The
    // presence of the type inside the optional indicates that the collection is involved in a
    // resharding operation, and that these fields were present in the config.collections entry
    // for this collection.
    boost::optional<TypeCollectionReshardingFields> _reshardingFields;

    bool _allowMigrations;

    // Map from the max for each chunk to an entry describing the chunk. The union of all chunks'
    // ranges must cover the complete space from [MinKey, MaxKey).
    ChunkMap _chunkMap;
};

/**
 * Constructed to be used exclusively by the CatalogCache as a vector clock (Time) to drive
 * CollectionCache's lookups.
 *
 * The ChunkVersion class contains a timestamp for the collection generation which resets to 0 after
 * the collection is dropped or all chunks are moved off of a shard, in which case the versions
 * cannot be compared.
 *
 * This class wraps a ChunkVersion object with a node-local sequence number
 * (_epochDisambiguatingSequenceNum) that allows the comparision.
 *
 * This class should go away once a cluster-wide comparable ChunkVersion is implemented.
 */
class ComparableChunkVersion {
public:
    /**
     * Creates a ComparableChunkVersion that wraps the given ChunkVersion.
     * Each object created through this method will have a local sequence number greater than the
     * previously created ones.
     */
    static ComparableChunkVersion makeComparableChunkVersion(const ChunkVersion& version);

    /**
     * Creates a new instance which will artificially be greater than any
     * previously created ComparableChunkVersion and smaller than any instance
     * created afterwards. Used as means to cause the collections cache to
     * attempt a refresh in situations where causal consistency cannot be
     * inferred.
     */
    static ComparableChunkVersion makeComparableChunkVersionForForcedRefresh();

    /**
     * Empty constructor needed by the ReadThroughCache.
     *
     * Instances created through this constructor will be always less then the ones created through
     * the two static constructors, but they do not carry any meaningful value and can only be used
     * for comparison purposes.
     */
    ComparableChunkVersion() = default;

    std::string toString() const;

    bool operator==(const ComparableChunkVersion& other) const;

    bool operator!=(const ComparableChunkVersion& other) const {
        return !(*this == other);
    }

    /**
     * In case the two compared instances have different epochs, the most recently created one will
     * be greater, otherwise the comparision will be driven by the major/minor versions of the
     * underlying ChunkVersion.
     */
    bool operator<(const ComparableChunkVersion& other) const;

    bool operator>(const ComparableChunkVersion& other) const {
        return other < *this;
    }

    bool operator<=(const ComparableChunkVersion& other) const {
        return !(*this > other);
    }

    bool operator>=(const ComparableChunkVersion& other) const {
        return !(*this < other);
    }

private:
    friend class CatalogCache;

    static AtomicWord<uint64_t> _epochDisambiguatingSequenceNumSource;
    static AtomicWord<uint64_t> _forcedRefreshSequenceNumSource;

    ComparableChunkVersion(uint64_t forcedRefreshSequenceNum,
                           boost::optional<ChunkVersion> version,
                           uint64_t epochDisambiguatingSequenceNum)
        : _forcedRefreshSequenceNum(forcedRefreshSequenceNum),
          _chunkVersion(std::move(version)),
          _epochDisambiguatingSequenceNum(epochDisambiguatingSequenceNum) {}

    void setChunkVersion(const ChunkVersion& version);

    uint64_t _forcedRefreshSequenceNum{0};

    boost::optional<ChunkVersion> _chunkVersion;

    // Locally incremented sequence number that allows to compare two colection versions with
    // different epochs. Each new comparableChunkVersion will have a greater sequence number than
    // the ones created before.
    uint64_t _epochDisambiguatingSequenceNum{0};
};

/**
 * This intermediate structure is necessary to be able to store UNSHARDED collections in the routing
 * table history cache below. The reason is that currently the RoutingTableHistory class only
 * supports sharded collections (i.e., collections which have entries in config.collections and
 * config.chunks).
 */
struct OptionalRoutingTableHistory {
    // UNSHARDED collection constructor
    OptionalRoutingTableHistory() = default;

    // SHARDED collection constructor
    OptionalRoutingTableHistory(std::shared_ptr<RoutingTableHistory> rt) : optRt(std::move(rt)) {}

    // If nullptr, the collection is UNSHARDED, otherwise it is SHARDED
    std::shared_ptr<RoutingTableHistory> optRt;
};

using RoutingTableHistoryCache =
    ReadThroughCache<NamespaceString, OptionalRoutingTableHistory, ComparableChunkVersion>;
using RoutingTableHistoryValueHandle = RoutingTableHistoryCache::ValueHandle;

/**
 * Combines a shard, the shard version, and database version that the shard should be using
 */
struct ShardEndpoint {
    ShardEndpoint(const ShardId& shardName,
                  boost::optional<ShardVersion> shardVersionParam,
                  boost::optional<DatabaseVersion> dbVersionParam);

    ShardId shardName;

    boost::optional<ShardVersion> shardVersion;
    boost::optional<DatabaseVersion> databaseVersion;
};

/**
 * Compares shard endpoints in a map.
 */
struct EndpointComp {
    bool operator()(const ShardEndpoint* endpointA, const ShardEndpoint* endpointB) const;
};

/**
 * Wrapper around a RoutingTableHistory, which pins it to a particular point in time.
 */
class ChunkManager {
public:
    ChunkManager(ShardId dbPrimary,
                 DatabaseVersion dbVersion,
                 RoutingTableHistoryValueHandle rt,
                 boost::optional<Timestamp> clusterTime)
        : _dbPrimary(std::move(dbPrimary)),
          _dbVersion(std::move(dbVersion)),
          _rt(std::move(rt)),
          _clusterTime(std::move(clusterTime)) {}

    // Methods supported on both sharded and unsharded collections

    /*
     * Returns true if this chunk manager has a routing table.
     *
     * True for:
     *   - sharded collections.
     *   - unsharded collections tracked by the configsvr.
     * False for:
     *   - unsharded collections not tracked by the configsvr.
     *   - non-existent collections.
     */
    bool hasRoutingTable() const {
        return bool(_rt->optRt);
    }

    /*
     * Returns true if routing table is present and unsplittable flag is not set
     */
    bool isSharded() const {
        return hasRoutingTable() ? !_rt->optRt->_unsplittable : false;
    }

    /*
     * Returns true if routing table is present and unsplittable flag is set
     */
    bool isUnsplittable() const {
        return hasRoutingTable() ? _rt->optRt->_unsplittable : false;
    }

    bool isAtPointInTime() const {
        return bool(_clusterTime);
    }

    /**
     * Indicates that this collection must not honour any moveChunk requests, because it is required
     * to provide a stable view of its constituent shards.
     */
    bool allowMigrations() const;

    const ShardId& dbPrimary() const {
        return _dbPrimary;
    }

    const DatabaseVersion& dbVersion() const {
        return _dbVersion;
    }

    int numChunks() const {
        return _rt->optRt ? _rt->optRt->numChunks() : 1;
    }

    std::string toString() const;

    // Methods only supported on collections registered in the sharding catalog (caller must check
    // hasRoutingTable())

    const ShardKeyPattern& getShardKeyPattern() const {
        tassert(7626400, "Expected routing table to be initialized", _rt->optRt);
        return _rt->optRt->getShardKeyPattern();
    }

    const CollatorInterface* getDefaultCollator() const {
        tassert(7626401, "Expected routing table to be initialized", _rt->optRt);
        return _rt->optRt->getDefaultCollator();
    }

    bool isUnique() const {
        tassert(7626402, "Expected routing table to be initialized", _rt->optRt);
        return _rt->optRt->isUnique();
    }

    ChunkVersion getVersion() const {
        tassert(7626403, "Expected routing table to be initialized", _rt->optRt);
        return _rt->optRt->getVersion();
    }

    /**
     * Retrieves the placement version for the given shard.
     */
    ChunkVersion getVersion(const ShardId& shardId) const {
        tassert(7626404, "Expected routing table to be initialized", _rt->optRt);
        return _rt->optRt->getVersion(shardId);
    }

    /**
     * Retrieves the maximum validAfter timestamp for the given shard.
     */
    Timestamp getMaxValidAfter(const ShardId& shardId) const {
        tassert(7626405, "Expected routing table to be initialized", _rt->optRt);
        return _rt->optRt->getMaxValidAfter(shardId);
    }

    /**
     * Retrieves the placement version for the given shard.
     * Only use when logging the given chunk version -- if the caller must execute logic
     * based on the returned version, use getVersion() instead.
     */
    ChunkVersion getVersionForLogging(const ShardId& shardId) const {
        tassert(7626406, "Expected routing table to be initialized", _rt->optRt);
        return _rt->optRt->getVersionForLogging(shardId);
    }

    template <typename Callable>
    void forEachChunk(Callable&& handler, const BSONObj& shardKey = BSONObj()) const {
        tassert(7626407, "Expected routing table to be initialized", _rt->optRt);
        _rt->optRt->forEachChunk(
            [this, handler = std::forward<Callable>(handler)](const auto& chunkInfo) mutable {
                if (!handler(Chunk{*chunkInfo, _clusterTime}))
                    return false;

                return true;
            },
            shardKey);
    }

    template <typename Callable>
    void forEachOverlappingChunk(const BSONObj& min,
                                 const BSONObj& max,
                                 bool isMaxInclusive,
                                 Callable&& handler) const {
        _rt->optRt->forEachOverlappingChunk(
            min,
            max,
            isMaxInclusive,
            [this, handler = std::forward<Callable>(handler)](const auto& chunkInfo) mutable {
                if (!handler(Chunk{*chunkInfo, _clusterTime})) {
                    return false;
                }
                return true;
            });
    }

    /**
     * Returns true if a document with the given "shardKey" is owned by the shard with the given
     * "shardId" in this routing table. If "shardKey" is empty returns false. If "shardKey" is not a
     * valid shard key, the behaviour is undefined.
     */
    bool keyBelongsToShard(const BSONObj& shardKey, const ShardId& shardId) const;

    /**
     * Returns true if any chunk owned by the shard with the given "shardId" overlaps "range".
     */
    bool rangeOverlapsShard(const ChunkRange& range, const ShardId& shardId) const;

    /**
     * Given a shardKey, returns the first chunk which is owned by shardId and overlaps or sorts
     * after that shardKey. If the return value is empty, this means no such chunk exists.
     */
    boost::optional<Chunk> getNextChunkOnShard(const BSONObj& shardKey,
                                               const ShardId& shardId) const;

    /**
     * Given a shard key (or a prefix) that has been extracted from a document, returns the chunk
     * that contains that key.
     *
     * Example: findIntersectingChunk({a : hash('foo')}) locates the chunk for document
     *          {a: 'foo', b: 'bar'} if the shard key is {a : 'hashed'}.
     *
     * If 'collation' is empty, we use the collection default collation for targeting.
     *
     * Throws a DBException with the ShardKeyNotFound code if unable to target a single shard due to
     * collation or due to the key not matching the shard key pattern.
     */
    Chunk findIntersectingChunk(const BSONObj& shardKey,
                                const BSONObj& collation,
                                bool bypassIsFieldHashedCheck = false) const;

    /**
     * Same as findIntersectingChunk, but assumes the simple collation.
     */
    Chunk findIntersectingChunkWithSimpleCollation(const BSONObj& shardKey) const {
        tassert(7626408, "Expected routing table to be initialized", _rt->optRt);
        return findIntersectingChunk(shardKey, CollationSpec::kSimpleSpec);
    }

    /**
     * Finds the shard id of the shard that owns the chunk minKey belongs to, assuming the simple
     * collation because shard keys do not support non-simple collations.
     */
    ShardId getMinKeyShardIdWithSimpleCollation() const;

    /**
     * Returns all shard ids which contain chunks overlapping the range [min, max]. Please note the
     * inclusive bounds on both sides (SERVER-20768).
     * If 'chunkRanges' is not null, populates it with ChunkRanges that would be targeted by the
     * query.
     */
    void getShardIdsForRange(const BSONObj& min,
                             const BSONObj& max,
                             std::set<ShardId>* shardIds,
                             std::set<ChunkRange>* chunkRanges = nullptr,
                             bool includeMaxBound = true) const;

    /**
     * Returns the ids of all shards on which the collection has any chunks.
     */
    void getAllShardIds(std::set<ShardId>* all) const {
        tassert(7626409, "Expected routing table to be initialized", _rt->optRt);
        _rt->optRt->getAllShardIds(all);
    }

    /**
     * Returns the chunk ranges of all shards on which the collection has any chunks.
     */
    void getAllChunkRanges(std::set<ChunkRange>* all) const {
        tassert(7626410, "Expected routing table to be initialized", _rt->optRt);
        _rt->optRt->getAllChunkRanges(all);
    }

    /**
     * Returns the number of shards on which the collection has any chunks
     */
    size_t getNShardsOwningChunks() const {
        tassert(7626411, "Expected routing table to be initialized", _rt->optRt);
        return _rt->optRt->getNShardsOwningChunks();
    }

    /**
     * Constructs a new ChunkManager, which is a view of the underlying routing table at a different
     * `clusterTime`.
     */
    static ChunkManager makeAtTime(const ChunkManager& cm, Timestamp clusterTime);

    bool uuidMatches(const UUID& uuid) const {
        tassert(7626412, "Expected routing table to be initialized", _rt->optRt);
        return _rt->optRt->uuidMatches(uuid);
    }

    const UUID& getUUID() const {
        tassert(7626413, "Expected routing table to be initialized", _rt->optRt);
        return _rt->optRt->getUUID();
    }

    const NamespaceString& getNss() const {
        tassert(7626414, "Expected routing table to be initialized", _rt->optRt);
        return _rt->optRt->nss();
    }

    const boost::optional<TypeCollectionTimeseriesFields>& getTimeseriesFields() const {
        tassert(7626415, "Expected routing table to be initialized", _rt->optRt);
        return _rt->optRt->getTimeseriesFields();
    }

    const boost::optional<TypeCollectionReshardingFields>& getReshardingFields() const {
        tassert(7626416, "Expected routing table to be initialized", _rt->optRt);
        return _rt->optRt->getReshardingFields();
    }

    const RoutingTableHistory& getRoutingTableHistory_ForTest() const {
        tassert(7626417, "Expected routing table to be initialized", _rt->optRt);
        return *_rt->optRt;
    }

private:
    ShardId _dbPrimary;
    DatabaseVersion _dbVersion;

    RoutingTableHistoryValueHandle _rt;

    boost::optional<Timestamp> _clusterTime;
};

}  // namespace mongo
