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

#include "mongo/db/repl/optime.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/concurrency/ticketholder.h"

namespace mongo {

class CanonicalQuery;
class Chunk;
class ChunkManager;
class CollectionType;
struct QuerySolutionNode;
class OperationContext;

typedef std::shared_ptr<ChunkManager> ChunkManagerPtr;

// The key for the map is max for each Chunk or ChunkRange
typedef std::map<BSONObj, std::shared_ptr<Chunk>, BSONObjCmp> ChunkMap;

class ChunkManager {
public:
    typedef std::map<ShardId, ChunkVersion> ShardVersionMap;

    // Loads a new chunk manager from a collection document
    explicit ChunkManager(const CollectionType& coll);

    // Creates an empty chunk manager for the namespace
    ChunkManager(const std::string& ns, const ShardKeyPattern& pattern, bool unique);

    const std::string& getns() const {
        return _ns;
    }
    const ShardKeyPattern& getShardKeyPattern() const {
        return _keyPattern;
    }
    bool isUnique() const {
        return _unique;
    }

    /**
     * this is just an increasing number of how many ChunkManagers we have so we know if something
     * has been updated
     */
    unsigned long long getSequenceNumber() const {
        return _sequenceNumber;
    }

    //
    // After constructor is invoked, we need to call loadExistingRanges.  If this is a new
    // sharded collection, we can call createFirstChunks first.
    //

    // Creates new chunks based on info in chunk manager
    Status createFirstChunks(OperationContext* txn,
                             const ShardId& primaryShardId,
                             const std::vector<BSONObj>* initPoints,
                             const std::set<ShardId>* initShardIds);

    // Loads existing ranges based on info in chunk manager
    void loadExistingRanges(OperationContext* txn, const ChunkManager* oldManager);


    // Helpers for load
    void calcInitSplitsAndShards(OperationContext* txn,
                                 const ShardId& primaryShardId,
                                 const std::vector<BSONObj>* initPoints,
                                 const std::set<ShardId>* initShardIds,
                                 std::vector<BSONObj>* splitPoints,
                                 std::vector<ShardId>* shardIds) const;

    //
    // Methods to use once loaded / created
    //

    int numChunks() const {
        return _chunkMap.size();
    }

    /**
     * Given a key that has been extracted from a document, returns the
     * chunk that contains that key.
     *
     * For instance, to locate the chunk for document {a : "foo" , b : "bar"}
     * when the shard key is {a : "hashed"}, you can call
     *  findIntersectingChunk() on {a : hash("foo") }
     */
    std::shared_ptr<Chunk> findIntersectingChunk(OperationContext* txn,
                                                 const BSONObj& shardKey) const;

    void getShardIdsForQuery(OperationContext* txn,
                             const BSONObj& query,
                             std::set<ShardId>* shardIds) const;

    void getAllShardIds(std::set<ShardId>* all) const;

    /** @param shardIds set to the shard ids for shards
     *         covered by the interval [min, max], see SERVER-4791
     */
    void getShardIdsForRange(std::set<ShardId>& shardIds,
                             const BSONObj& min,
                             const BSONObj& max) const;

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

    const ChunkMap& getChunkMap() const {
        return _chunkMap;
    }

    /**
     * Returns true if, for this shard, the chunks are identical in both chunk managers
     */
    bool compatibleWith(const ChunkManager& other, const ShardId& shard) const;

    std::string toString() const;

    ChunkVersion getVersion(const ShardId& shardName) const;
    ChunkVersion getVersion() const;

    void _printChunks() const;

    uint64_t getCurrentDesiredChunkSize() const;

    std::shared_ptr<ChunkManager> reload(OperationContext* txn,
                                         bool force = true) const;  // doesn't modify self!

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

    // Contains a compressed map of what range of keys resides on which shard. The index is the max
    // key of the respective range and the union of all ranges in a such constructed map must cover
    // the complete space from [MinKey, MaxKey).
    using ChunkRangeMap = std::map<BSONObj, ShardAndChunkRange, BSONObjCmp>;

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

    // All members should be const for thread-safety
    const std::string _ns;
    const ShardKeyPattern _keyPattern;
    const bool _unique;

    // The shard versioning mechanism hinges on keeping track of the number of times we reload
    // ChunkManagers. Increasing this number here will prompt checkShardVersion to refresh the
    // connection-level versions to the most up to date value.
    const unsigned long long _sequenceNumber;

    ChunkMap _chunkMap;
    ChunkRangeMap _chunkRangeMap;

    std::set<ShardId> _shardIds;

    // Max known version per shard
    ShardVersionMap _shardVersions;

    // Max version across all chunks
    ChunkVersion _version;

    // OpTime of config server the last time chunks were loaded.
    repl::OpTime _configOpTime;

    //
    // Split Heuristic info
    //
    class SplitHeuristics {
    public:
        SplitHeuristics() : _splitTickets(maxParallelSplits) {}

        TicketHolder _splitTickets;

        // Test whether we should split once data * splitTestFactor > chunkSize (approximately)
        static const int splitTestFactor = 5;
        // Maximum number of parallel threads requesting a split
        static const int maxParallelSplits = 5;

        // The idea here is that we're over-aggressive on split testing by a factor of
        // splitTestFactor, so we can safely wait until we get to splitTestFactor invalid splits
        // before changing.  Unfortunately, we also potentially over-request the splits by a
        // factor of maxParallelSplits, but since the factors are identical it works out
        // (for now) for parallel or sequential oversplitting.
        // TODO: Make splitting a separate thread with notifications?
        static const int staleMinorReloadThreshold = maxParallelSplits;
    };

    mutable SplitHeuristics _splitHeuristics;

    //
    // End split heuristics
    //

    friend class Chunk;
    static AtomicUInt32 NextSequenceNumber;

    friend class TestableChunkManager;
};

}  // namespace mongo
