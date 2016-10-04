/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <boost/optional.hpp>

#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"

namespace mongo {

class ChunkManager;
class ChunkType;
class OperationContext;

/**
   config.chunks
   { ns : "alleyinsider.fs.chunks" , min : {} , max : {} , server : "localhost:30001" }

   x is in a shard iff
   min <= x < max
 */
class Chunk {
    MONGO_DISALLOW_COPYING(Chunk);

public:
    enum SplitPointMode {
        // Determines the split points that will make the current chunk smaller than
        // the current chunk size setting. Gives empty result if chunk is not big enough.
        normal,

        // Will get a split which approximately splits the chunk into 2 halves,
        // regardless of the size of the chunk.
        atMedian,

        // Behaves like normal, with additional special heuristics for "top chunks"
        // (the 1 or 2 chunks in the extreme ends of the chunk key space).
        autoSplitInternal
    };

    Chunk(OperationContext* txn, ChunkManager* manager, const ChunkType& from);

    Chunk(ChunkManager* manager,
          const BSONObj& min,
          const BSONObj& max,
          const ShardId& shardId,
          ChunkVersion lastmod,
          uint64_t initialDataWritten);

    //
    // chunk boundary support
    //

    const BSONObj& getMin() const {
        return _min;
    }

    const BSONObj& getMax() const {
        return _max;
    }

    // Returns true if this chunk contains the given shard key, and false otherwise
    //
    // Note: this function takes an extracted *key*, not an original document
    // (the point may be computed by, say, hashing a given field or projecting
    //  to a subset of fields).
    bool containsKey(const BSONObj& shardKey) const;

    //
    // chunk version support
    //

    void appendShortVersion(const char* name, BSONObjBuilder& b) const;

    ChunkVersion getLastmod() const {
        return _lastmod;
    }

    //
    // split support
    //

    long long getBytesWritten() const {
        return _dataWritten;
    }

    /**
     * if the amount of data written nears the max size of a shard
     * then we check the real size, and if its too big, we split
     * @return if something was split
     */
    bool splitIfShould(OperationContext* txn, long dataWritten);

    /**
     * Splits this chunk at a non-specificed split key to be chosen by the
     * mongod holding this chunk.
     *
     * @param mode
     * @param res the object containing details about the split execution
     * @param resultingSplits the number of resulting split points. Set to NULL to ignore.
     *
     * @throws UserException
     */
    StatusWith<boost::optional<ChunkRange>> split(OperationContext* txn,
                                                  SplitPointMode mode,
                                                  size_t* resultingSplits) const;

    /**
     * marks this chunk as a jumbo chunk
     * that means the chunk will be inelligble for migrates
     */
    void markAsJumbo(OperationContext* txn) const;

    bool isJumbo() const {
        return _jumbo;
    }

    //
    // public constants
    //

    static const int MaxObjectPerChunk{250000};

    //
    // accessors and helpers
    //

    std::string toString() const;

    friend std::ostream& operator<<(std::ostream& out, const Chunk& c) {
        return (out << c.toString());
    }

    // chunk equality is determined by comparing the min and max bounds of the chunk
    bool operator==(const Chunk& s) const;
    bool operator!=(const Chunk& s) const {
        return !(*this == s);
    }

    ShardId getShardId() const {
        return _shardId;
    }
    const ChunkManager* getManager() const {
        return _manager;
    }

private:
    /**
     * Returns the connection string for the shard on which this chunk resides.
     */
    ConnectionString _getShardConnectionString(OperationContext* txn) const;

    // if min/max key is pos/neg infinity
    bool _minIsInf() const;
    bool _maxIsInf() const;

    // The chunk manager, which owns this chunk. Not owned by the chunk.
    const ChunkManager* _manager;

    BSONObj _min;
    BSONObj _max;
    ShardId _shardId;
    const ChunkVersion _lastmod;
    mutable bool _jumbo;

    // Statistics for the approximate data written by this chunk
    uint64_t _dataWritten;

    // methods, etc..

    /**
     * Returns the split point that will result in one of the chunk having exactly one
     * document. Also returns an empty document if the split point cannot be determined.
     *
     * @param doSplitAtLower determines which side of the split will have exactly one document.
     *        True means that the split point chosen will be closer to the lower bound.
     *
     * Warning: this assumes that the shard key is not "special"- that is, the shardKeyPattern
     *          is simply an ordered list of ascending/descending field names. Examples:
     *          {a : 1, b : -1} is not special. {a : "hashed"} is.
     */
    BSONObj _getExtremeKey(OperationContext* txn, bool doSplitAtLower) const;

    /**
     * Determines the appropriate split points for this chunk.
     *
     * @param atMedian perform a single split at the middle of this chunk.
     * @param splitPoints out parameter containing the chosen split points. Can be empty.
     */
    std::vector<BSONObj> _determineSplitPoints(OperationContext* txn, bool atMedian) const;

    /**
     * initializes _dataWritten with a random value so that a mongos restart
     * wouldn't cause delay in splitting
     */
    static int mkDataWritten();
};

}  // namespace mongo
