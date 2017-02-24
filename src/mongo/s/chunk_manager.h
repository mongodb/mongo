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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/optime.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/concurrency/ticketholder.h"

namespace mongo {

class CanonicalQuery;
struct QuerySolutionNode;
class OperationContext;

// Ordered map from the max for each chunk to an entry describing the chunk
using ChunkMap = BSONObjIndexedMap<std::shared_ptr<Chunk>>;

// Map from a shard is to the max chunk version on that shard
using ShardVersionMap = std::map<ShardId, ChunkVersion>;

class ChunkManager {
    MONGO_DISALLOW_COPYING(ChunkManager);

public:
    ChunkManager(NamespaceString nss,
                 const OID& epoch,
                 const ShardKeyPattern& shardKeyPattern,
                 std::unique_ptr<CollatorInterface> defaultCollator,
                 bool unique);

    ~ChunkManager();

    /**
     * Returns an increasing number of the reload sequence number of this chunk manager.
     */
    unsigned long long getSequenceNumber() const {
        return _sequenceNumber;
    }

    const std::string& getns() const {
        return _nss.ns();
    }

    const ShardKeyPattern& getShardKeyPattern() const {
        return _keyPattern;
    }

    const CollatorInterface* getDefaultCollator() const {
        return _defaultCollator.get();
    }

    bool isUnique() const {
        return _unique;
    }

    ChunkVersion getVersion() const {
        return _version;
    }

    const ChunkMap& getChunkMap() const {
        return _chunkMap;
    }

    int numChunks() const {
        return _chunkMap.size();
    }

    // Loads existing ranges based on info in chunk manager
    void loadExistingRanges(OperationContext* txn, const ChunkManager* oldManager);

    //
    // Methods to use once loaded / created
    //

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
    std::shared_ptr<Chunk> findIntersectingChunk(const BSONObj& shardKey,
                                                 const BSONObj& collation) const;

    /**
     * Same as findIntersectingChunk, but assumes the simple collation.
     */
    std::shared_ptr<Chunk> findIntersectingChunkWithSimpleCollation(const BSONObj& shardKey) const;

    /**
     * Finds the shard IDs for a given filter and collation. If collation is empty, we use the
     * collection default collation for targeting.
     */
    void getShardIdsForQuery(OperationContext* txn,
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
    void getAllShardIds(std::set<ShardId>* all) const;

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
    bool compatibleWith(const ChunkManager& other, const ShardId& shard) const;

    std::string toString() const;

    ChunkVersion getVersion(const ShardId& shardName) const;

    /**
     * Returns the opTime of config server the last time chunks were loaded.
     */
    repl::OpTime getConfigOpTime() const;

private:
    /**
     * Represents a range of chunk keys [getMin(), getMax()) and the id of the shard on which they
     * reside according to the metadata.
     */
    class ShardAndChunkRange {
    public:
        ShardAndChunkRange(const BSONObj& min, const BSONObj& max, ShardId inShardId)
            : _range(min, max), _shardId(std::move(inShardId)) {}

        const BSONObj& getMin() const {
            return _range.getMin();
        }

        const BSONObj& getMax() const {
            return _range.getMax();
        }

        const ShardId& getShardId() const {
            return _shardId;
        }

    private:
        ChunkRange _range;
        ShardId _shardId;
    };

    using ChunkRangeMap = BSONObjIndexedMap<ShardAndChunkRange>;

    /**
     * If load was successful, returns true and it is guaranteed that the _chunkMap and
     * _chunkRangeMap are consistent with each other. If false is returned, it is not safe to use
     * the chunk manager anymore.
     */
    bool _load(OperationContext* txn,
               ChunkMap& chunks,
               std::set<ShardId>& shardIds,
               ShardVersionMap* shardVersions,
               const ChunkManager* oldManager);

    /**
     * Merges consecutive chunks, which reside on the same shard into a single range.
     */
    static ChunkRangeMap _constructRanges(const ChunkMap& chunkMap);

    // The shard versioning mechanism hinges on keeping track of the number of times we reload
    // ChunkManagers.
    const unsigned long long _sequenceNumber;

    // Namespace to which this routing information corresponds
    const NamespaceString _nss;

    // The key pattern used to shard the collection
    const ShardKeyPattern _keyPattern;

    // Default collation to use for routing data queries for this collection
    const std::unique_ptr<CollatorInterface> _defaultCollator;

    // Whether the sharding key is unique
    const bool _unique;

    // Map from the max for each chunk to an entry describing the chunk. The union of all chunks'
    // ranges must cover the complete space from [MinKey, MaxKey).
    ChunkMap _chunkMap;

    // Transformation of the chunk map containing what range of keys reside on which shard. The
    // index is the max key of the respective range and the union of all ranges in a such
    // constructed map must cover the complete space from [MinKey, MaxKey).
    ChunkRangeMap _chunkRangeMap;

    // Max known version per shard
    ShardVersionMap _shardVersions;

    // Max version across all chunks
    ChunkVersion _version;

    // OpTime of config server the last time chunks were loaded.
    repl::OpTime _configOpTime;

    // Auto-split throttling state
    struct AutoSplitThrottle {
    public:
        AutoSplitThrottle() : _splitTickets(maxParallelSplits) {}

        TicketHolder _splitTickets;

        // Maximum number of parallel threads requesting a split
        static const int maxParallelSplits = 5;

    } _autoSplitThrottle;

    // This function needs to be able to access the auto-split throttle
    friend void updateChunkWriteStatsAndSplitIfNeeded(OperationContext*,
                                                      ChunkManager*,
                                                      Chunk*,
                                                      long);

    friend class TestableChunkManager;
};

}  // namespace mongo
