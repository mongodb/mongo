/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/concurrency/ticketholder.h"

namespace mongo {

class CanonicalQuery;
struct QuerySolutionNode;
class OperationContext;
class ChunkManager;

// Ordered map from the max for each chunk to an entry describing the chunk
using ChunkInfoMap = std::map<std::string, std::shared_ptr<ChunkInfo>>;

// Map from a shard is to the max chunk version on that shard
using ShardVersionMap = std::map<ShardId, ChunkVersion>;

/**
 * In-memory representation of the routing table for a single sharded collection at various points
 * in time.
 */
class RoutingTableHistory : public std::enable_shared_from_this<RoutingTableHistory> {
    MONGO_DISALLOW_COPYING(RoutingTableHistory);

public:
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
     */
    static std::shared_ptr<RoutingTableHistory> makeNew(
        NamespaceString nss,
        boost::optional<UUID>,
        KeyPattern shardKeyPattern,
        std::unique_ptr<CollatorInterface> defaultCollator,
        bool unique,
        OID epoch,
        const std::vector<ChunkType>& chunks);

    /**
     * Constructs a new instance with a routing table updated according to the changes described
     * in "changedChunks".
     *
     * The changes in "changedChunks" must be sorted in ascending order by chunk version, and adhere
     * to the requirements of the routing table update algorithm.
     */
    std::shared_ptr<RoutingTableHistory> makeUpdated(const std::vector<ChunkType>& changedChunks);

    /**
     * Returns an increasing number of the reload sequence number of this chunk manager.
     */
    unsigned long long getSequenceNumber() const {
        return _sequenceNumber;
    }

    const NamespaceString& getns() const {
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

    ChunkVersion getVersion() const {
        return _collectionVersion;
    }

    ChunkVersion getVersion(const ShardId& shardId) const;

    const ChunkInfoMap& getChunkMap() const {
        return _chunkMap;
    }

    /**
     * Returns the ids of all shards on which the collection has any chunks.
     */
    void getAllShardIds(std::set<ShardId>* all) const;

    /**
     * Returns true if, for this shard, the chunks are identical in both chunk managers
     */
    bool compatibleWith(const RoutingTableHistory& other, const ShardId& shard) const;

    std::string toString() const;

    bool uuidMatches(UUID uuid) const {
        return _uuid && *_uuid == uuid;
    }

    boost::optional<UUID> getUUID() const {
        return _uuid;
    }

    std::pair<ChunkInfoMap::const_iterator, ChunkInfoMap::const_iterator> overlappingRanges(
        const BSONObj& min, const BSONObj& max, bool isMaxInclusive) const;


private:
    /**
     * Does a single pass over the chunkMap and constructs the ShardVersionMap object.
     */
    static ShardVersionMap _constructShardVersionMap(const OID& epoch,
                                                     const ChunkInfoMap& chunkMap,
                                                     Ordering shardKeyOrdering);

    RoutingTableHistory(NamespaceString nss,
                        boost::optional<UUID> uuid,
                        KeyPattern shardKeyPattern,
                        std::unique_ptr<CollatorInterface> defaultCollator,
                        bool unique,
                        ChunkInfoMap chunkMap,
                        ChunkVersion collectionVersion);

    std::string _extractKeyString(const BSONObj& shardKeyValue) const;

    // The shard versioning mechanism hinges on keeping track of the number of times we reload
    // ChunkManagers.
    const unsigned long long _sequenceNumber;

    // Namespace to which this routing information corresponds
    const NamespaceString _nss;

    // The invariant UUID of the collection.  This is optional in 3.6, except in change streams.
    const boost::optional<UUID> _uuid;

    // The key pattern used to shard the collection
    const ShardKeyPattern _shardKeyPattern;

    const Ordering _shardKeyOrdering;

    // Default collation to use for routing data queries for this collection
    const std::unique_ptr<CollatorInterface> _defaultCollator;

    // Whether the sharding key is unique
    const bool _unique;

    // Map from the max for each chunk to an entry describing the chunk. The union of all chunks'
    // ranges must cover the complete space from [MinKey, MaxKey).
    const ChunkInfoMap _chunkMap;

    // Map from shard id to the maximum chunk version for that shard. If a shard contains no
    // chunks, it won't be present in this map.
    const ShardVersionMap _shardVersions;

    // Max version across all chunks
    const ChunkVersion _collectionVersion;

    // Auto-split throttling state (state mutable by write commands)
    struct AutoSplitThrottle {
    public:
        AutoSplitThrottle() : _splitTickets(maxParallelSplits) {}

        TicketHolder _splitTickets;

        // Maximum number of parallel threads requesting a split
        static const int maxParallelSplits = 5;

    } _autoSplitThrottle;

    friend class ChunkManager;
    // This function needs to be able to access the auto-split throttle
    friend void updateChunkWriteStatsAndSplitIfNeeded(OperationContext*,
                                                      ChunkManager*,
                                                      Chunk,
                                                      long);
};

// This will be renamed to RoutingTableHistory and the original RoutingTableHistory will be
// ChunkHistoryMap
class ChunkManager : public std::enable_shared_from_this<ChunkManager> {
    MONGO_DISALLOW_COPYING(ChunkManager);

public:
    class ConstChunkIterator {
    public:
        ConstChunkIterator() = default;
        explicit ConstChunkIterator(ChunkInfoMap::const_iterator iter,
                                    const boost::optional<Timestamp>& clusterTime)
            : _iter{iter} {}

        ConstChunkIterator& operator++() {
            ++_iter;
            return *this;
        }
        ConstChunkIterator operator++(int) {
            return ConstChunkIterator{_iter++, _clusterTime};
        }
        bool operator==(const ConstChunkIterator& other) const {
            return _iter == other._iter;
        }
        bool operator!=(const ConstChunkIterator& other) const {
            return !(*this == other);
        }
        const Chunk operator*() const {
            return Chunk{*_iter->second, _clusterTime};
        }

    private:
        ChunkInfoMap::const_iterator _iter;
        const boost::optional<Timestamp> _clusterTime;
    };

    class ConstRangeOfChunks {
    public:
        ConstRangeOfChunks(ConstChunkIterator begin, ConstChunkIterator end)
            : _begin{std::move(begin)}, _end{std::move(end)} {}

        ConstChunkIterator begin() const {
            return _begin;
        }
        ConstChunkIterator end() const {
            return _end;
        }

    private:
        ConstChunkIterator _begin;
        ConstChunkIterator _end;
    };

    ChunkManager(std::shared_ptr<RoutingTableHistory> rt, boost::optional<Timestamp> clusterTime)
        : _rt(std::move(rt)), _clusterTime(std::move(clusterTime)) {}
    /**
     * Returns an increasing number of the reload sequence number of this chunk manager.
     */
    unsigned long long getSequenceNumber() const {
        return _rt->getSequenceNumber();
    }

    const NamespaceString& getns() const {
        return _rt->getns();
    }

    const ShardKeyPattern& getShardKeyPattern() const {
        return _rt->getShardKeyPattern();
    }

    const CollatorInterface* getDefaultCollator() const {
        return _rt->getDefaultCollator();
    }

    bool isUnique() const {
        return _rt->isUnique();
    }

    ChunkVersion getVersion() const {
        return _rt->getVersion();
    }

    ChunkVersion getVersion(const ShardId& shardId) const {
        return _rt->getVersion(shardId);
    }

    ConstRangeOfChunks chunks() const {
        return {ConstChunkIterator{_rt->getChunkMap().cbegin(), _clusterTime},
                ConstChunkIterator{_rt->getChunkMap().cend(), _clusterTime}};
    }

    int numChunks() const {
        return _rt->getChunkMap().size();
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
     * after that shardKey. The returned iterator range always contains one or zero entries. If zero
     * entries are returned, this means no such chunk exists.
     */
    ConstRangeOfChunks getNextChunkOnShard(const BSONObj& shardKey, const ShardId& shardId) const;

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
    Chunk findIntersectingChunk(const BSONObj& shardKey, const BSONObj& collation) const;

    /**
     * Same as findIntersectingChunk, but assumes the simple collation.
     */
    Chunk findIntersectingChunkWithSimpleCollation(const BSONObj& shardKey) const {
        return findIntersectingChunk(shardKey, CollationSpec::kSimpleSpec);
    }

    /**
     * Finds the shard IDs for a given filter and collation. If collation is empty, we use the
     * collection default collation for targeting.
     */
    void getShardIdsForQuery(OperationContext* opCtx,
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
        _rt->getAllShardIds(all);
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
     * Returns true if, for this shard, the chunks are identical in both chunk managers
     */
    bool compatibleWith(const ChunkManager& other, const ShardId& shard) const {
        return _rt->compatibleWith(*other._rt, shard);
    }

    std::string toString() const {
        return _rt->toString();
    }

    bool uuidMatches(UUID uuid) const {
        return _rt->uuidMatches(uuid);
    }

    auto& autoSplitThrottle() const {
        return _rt->_autoSplitThrottle;
    }

    RoutingTableHistory& getRoutingHistory() const {
        return *_rt;
    }

    boost::optional<UUID> getUUID() const {
        return _rt->getUUID();
    }

private:
    std::shared_ptr<RoutingTableHistory> _rt;
    boost::optional<Timestamp> _clusterTime;
};

}  // namespace mongo
