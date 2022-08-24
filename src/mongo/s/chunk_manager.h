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
#include <string>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/s/chunk.h"
#include "mongo/s/database_version.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/read_through_cache.h"

namespace mongo {

class CanonicalQuery;
struct QuerySolutionNode;
class ChunkManager;

struct ShardVersionTargetingInfo {
    // Indicates whether the shard is stale and thus needs a catalog cache refresh
    AtomicWord<bool> isStale{false};

    // Max chunk version for the shard
    ChunkVersion shardVersion;

    ShardVersionTargetingInfo(const OID& epoch, const Timestamp& timestamp);
};

// Map from a shard to a struct indicating both the max chunk version on that shard and whether the
// shard is currently marked as needing a catalog cache refresh (stale).
using ShardVersionMap = stdx::unordered_map<ShardId, ShardVersionTargetingInfo, ShardId::Hasher>;

/**
 * This class serves as a Facade around how the mapping of ranges to chunks is represented. It also
 * provides a simpler, high-level interface for domain specific operations without exposing the
 * underlying implementation.
 */
class ChunkMap {
    // Vector of chunks ordered by max key.
    using ChunkVector = std::vector<std::shared_ptr<ChunkInfo>>;

public:
    ChunkMap(OID epoch, const Timestamp& timestamp, size_t initialCapacity = 0);

    size_t size() const {
        return _chunkMap.size();
    }

    ChunkVersion getVersion() const {
        return _collectionVersion;
    }

    template <typename Callable>
    void forEach(Callable&& handler, const BSONObj& shardKey = BSONObj()) const {
        auto it = shardKey.isEmpty() ? _chunkMap.begin() : _findIntersectingChunk(shardKey);

        for (; it != _chunkMap.end(); ++it) {
            if (!handler(*it))
                break;
        }
    }

    template <typename Callable>
    void forEachOverlappingChunk(const BSONObj& min,
                                 const BSONObj& max,
                                 bool isMaxInclusive,
                                 Callable&& handler) const {
        const auto bounds = _overlappingBounds(min, max, isMaxInclusive);

        for (auto it = bounds.first; it != bounds.second; ++it) {
            if (!handler(*it))
                break;
        }
    }

    ShardVersionMap constructShardVersionMap() const;
    std::shared_ptr<ChunkInfo> findIntersectingChunk(const BSONObj& shardKey) const;

    void appendChunk(const std::shared_ptr<ChunkInfo>& chunk);

    ChunkMap createMerged(const std::vector<std::shared_ptr<ChunkInfo>>& changedChunks) const;

    BSONObj toBSON() const;

private:
    ChunkVector::const_iterator _findIntersectingChunk(const BSONObj& shardKey,
                                                       bool isMaxInclusive = true) const;
    std::pair<ChunkVector::const_iterator, ChunkVector::const_iterator> _overlappingBounds(
        const BSONObj& min, const BSONObj& max, bool isMaxInclusive) const;

    ChunkVector _chunkMap;

    // Max version across all chunks
    ChunkVersion _collectionVersion;
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
        std::unique_ptr<CollatorInterface> defaultCollator,
        bool unique,
        OID epoch,
        const Timestamp& timestamp,
        boost::optional<TypeCollectionTimeseriesFields> timeseriesFields,
        boost::optional<TypeCollectionReshardingFields> reshardingFields,
        boost::optional<uint64_t> maxChunkSizeBytes,
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
        boost::optional<uint64_t> maxChunkSizeBytes,
        bool allowMigrations,
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
     * Mark the given shard as stale, indicating that requests targetted to this shard (for this
     * namespace) need to block on a catalog cache refresh.
     */
    void setShardStale(const ShardId& shardId);

    /**
     * Mark all shards as not stale, indicating that a refresh has happened and requests targeted
     * to all shards (for this namespace) do not currently need to block on a catalog cache refresh.
     */
    void setAllShardsRefreshed();

    ChunkVersion getVersion() const {
        return _chunkMap.getVersion();
    }

    /**
     * Retrieves the shard version for the given shard. Will throw a ShardInvalidatedForTargeting
     * exception if the shard is marked as stale.
     */
    ChunkVersion getVersion(const ShardId& shardId) const;

    /**
     * Retrieves the shard version for the given shard. Will not throw if the shard is marked as
     * stale. Only use when logging the given chunk version -- if the caller must execute logic
     * based on the returned version, use getVersion() instead.
     */
    ChunkVersion getVersionForLogging(const ShardId& shardId) const;

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
     * Returns the number of shards on which the collection has any chunks
     */
    int getNShardsOwningChunks() const;

    /**
     * Returns true if, for this shard, the chunks are identical in both chunk managers
     */
    bool compatibleWith(const RoutingTableHistory& other, const ShardId& shard) const;

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

    // collection default chunk size or +inf, iff no splits should happen
    boost::optional<uint64_t> maxChunkSizeBytes() const {
        return _maxChunkSizeBytes;
    }

private:
    friend class ChunkManager;

    RoutingTableHistory(NamespaceString nss,
                        UUID uuid,
                        KeyPattern shardKeyPattern,
                        std::unique_ptr<CollatorInterface> defaultCollator,
                        bool unique,
                        boost::optional<TypeCollectionTimeseriesFields> timeseriesFields,
                        boost::optional<TypeCollectionReshardingFields> reshardingFields,
                        boost::optional<uint64_t> maxChunkSizeBytes,
                        bool allowMigrations,
                        ChunkMap chunkMap);

    ChunkVersion _getVersion(const ShardId& shardName, bool throwOnStaleShard) const;

    // Namespace to which this routing information corresponds
    NamespaceString _nss;

    // The UUID of the collection
    UUID _uuid;

    // The key pattern used to shard the collection
    ShardKeyPattern _shardKeyPattern;

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

    boost::optional<uint64_t> _maxChunkSizeBytes;

    bool _allowMigrations;

    // Map from the max for each chunk to an entry describing the chunk. The union of all chunks'
    // ranges must cover the complete space from [MinKey, MaxKey).
    ChunkMap _chunkMap;

    // The representation of shard versions and staleness indicators for this namespace. If a
    // shard does not exist, it will not have an entry in the map.
    // Note: this declaration must not be moved before _chunkMap since it is initialized by using
    // the _chunkMap instance.
    ShardVersionMap _shardVersions;
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
                  boost::optional<ChunkVersion> shardVersion,
                  boost::optional<DatabaseVersion> dbVersion);

    ShardId shardName;

    boost::optional<ChunkVersion> shardVersion;
    boost::optional<DatabaseVersion> databaseVersion;
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

    bool isSharded() const {
        return bool(_rt->optRt);
    }

    /**
     * Indicates that this collection must not honour any moveChunk requests, because it is required
     * to provide a stable view of its constituent shards.
     */
    bool allowMigrations() const;

    bool allowAutoSplit() const;

    boost::optional<uint64_t> maxChunkSizeBytes() const;

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

    // Methods only supported on sharded collections (caller must check isSharded())

    const ShardKeyPattern& getShardKeyPattern() const {
        return _rt->optRt->getShardKeyPattern();
    }

    const CollatorInterface* getDefaultCollator() const {
        return _rt->optRt->getDefaultCollator();
    }

    bool isUnique() const {
        return _rt->optRt->isUnique();
    }

    ChunkVersion getVersion() const {
        return _rt->optRt->getVersion();
    }

    ChunkVersion getVersion(const ShardId& shardId) const {
        return _rt->optRt->getVersion(shardId);
    }

    ChunkVersion getVersionForLogging(const ShardId& shardId) const {
        return _rt->optRt->getVersionForLogging(shardId);
    }

    template <typename Callable>
    void forEachChunk(Callable&& handler) const {
        _rt->optRt->forEachChunk(
            [this, handler = std::forward<Callable>(handler)](const auto& chunkInfo) mutable {
                if (!handler(Chunk{*chunkInfo, _clusterTime}))
                    return false;

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
        return findIntersectingChunk(shardKey, CollationSpec::kSimpleSpec);
    }

    /**
     * Finds the shard id of the shard that owns the chunk minKey belongs to, assuming the simple
     * collation because shard keys do not support non-simple collations.
     */
    ShardId getMinKeyShardIdWithSimpleCollation() const;

    /**
     * Finds the shard IDs for a given filter and collation. If collation is empty, we use the
     * collection default collation for targeting.
     */
    void getShardIdsForQuery(boost::intrusive_ptr<ExpressionContext> expCtx,
                             const BSONObj& query,
                             const BSONObj& collation,
                             std::set<ShardId>* shardIds) const;

    /**
     * Returns all shard ids which contain chunks overlapping the range [min, max]. Please note the
     * inclusive bounds on both sides (SERVER-20768).
     */
    void getShardIdsForRange(const BSONObj& min,
                             const BSONObj& max,
                             std::set<ShardId>* shardIds) const;

    /**
     * Returns the ids of all shards on which the collection has any chunks.
     */
    void getAllShardIds(std::set<ShardId>* all) const {
        _rt->optRt->getAllShardIds(all);
    }

    /**
     * Returns the number of shards on which the collection has any chunks
     */
    int getNShardsOwningChunks() const {
        return _rt->optRt->getNShardsOwningChunks();
    }

    // Transforms query into bounds for each field in the shard key
    // for example :
    //   Key { a: 1, b: 1 },
    //   Query { a : { $gte : 1, $lt : 2 },
    //            b : { $gte : 3, $lt : 4 } }
    //   => Bounds { a : [1, 2), b : [3, 4) }
    static IndexBounds getIndexBoundsForQuery(const BSONObj& key,
                                              const CanonicalQuery& canonicalQuery);

    // Collapse query solution tree.
    //
    // If it has OR node, the result could be a superset of the index bounds generated.
    // Since to give a single IndexBounds, this gives the union of bounds on each field.
    // for example:
    //   OR: { a: (0, 1), b: (0, 1) },
    //       { a: (2, 3), b: (2, 3) }
    //   =>  { a: (0, 1), (2, 3), b: (0, 1), (2, 3) }
    static IndexBounds collapseQuerySolution(const QuerySolutionNode* node);

    /**
     * Constructs a new ChunkManager, which is a view of the underlying routing table at a different
     * `clusterTime`.
     */
    static ChunkManager makeAtTime(const ChunkManager& cm, Timestamp clusterTime);

    /**
     * Returns true if, for this shard, the chunks are identical in both chunk managers
     */
    bool compatibleWith(const ChunkManager& other, const ShardId& shard) const {
        return _rt->optRt->compatibleWith(*other._rt->optRt, shard);
    }

    bool uuidMatches(const UUID& uuid) const {
        return _rt->optRt->uuidMatches(uuid);
    }

    const UUID& getUUID() const {
        return _rt->optRt->getUUID();
    }

    const boost::optional<TypeCollectionTimeseriesFields>& getTimeseriesFields() const {
        return _rt->optRt->getTimeseriesFields();
    }

    const boost::optional<TypeCollectionReshardingFields>& getReshardingFields() const {
        return _rt->optRt->getReshardingFields();
    }

    const RoutingTableHistory& getRoutingTableHistory_ForTest() const {
        return *_rt->optRt;
    }

private:
    ShardId _dbPrimary;
    DatabaseVersion _dbVersion;

    RoutingTableHistoryValueHandle _rt;

    boost::optional<Timestamp> _clusterTime;
};

}  // namespace mongo
