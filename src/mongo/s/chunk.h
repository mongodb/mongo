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

#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/shard_id.h"

namespace mongo {

class BSONObj;

/**
 * Represents a cache entry for a single Chunk. Owned by a ChunkManager.
 */
class Chunk {
public:
    // Test whether we should split once data * kSplitTestFactor > chunkSize (approximately)
    const uint64_t kSplitTestFactor = 5;

    explicit Chunk(const ChunkType& from);

    const BSONObj& getMin() const {
        return _range.getMin();
    }

    const BSONObj& getMax() const {
        return _range.getMax();
    }

    const ShardId& getShardId() const {
        return _shardId;
    }

    ChunkVersion getLastmod() const {
        return _lastmod;
    }

    bool isJumbo() const {
        return _jumbo;
    }

    /**
     * Returns a string represenation of the chunk for logging.
     */
    std::string toString() const;

    // Returns true if this chunk contains the given shard key, and false otherwise
    //
    // Note: this function takes an extracted *key*, not an original document (the point may be
    // computed by, say, hashing a given field or projecting to a subset of fields).
    bool containsKey(const BSONObj& shardKey) const;

    /**
     * Get/increment/set the estimation of how much data was written for this chunk.
     */
    uint64_t getBytesWritten() const;
    uint64_t addBytesWritten(uint64_t bytesWrittenIncrement);
    void clearBytesWritten();

    bool shouldSplit(uint64_t desiredChunkSize, bool minIsInf, bool maxIsInf) const;

    /**
     * Marks this chunk as jumbo. Only moves from false to true once and is used by the balancer.
     */
    void markAsJumbo();

private:
    const ChunkRange _range;

    const ShardId _shardId;

    const ChunkVersion _lastmod;

    // Indicates whether this chunk should be treated as jumbo and not attempted to be moved or
    // split
    mutable bool _jumbo;

    // Statistics for the approximate data written to this chunk
    mutable uint64_t _dataWritten;
};

}  // namespace mongo
