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

#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/shard_id.h"

namespace mongo {

class BSONObj;
class ChunkWritesTracker;

/**
 * Represents a cache entry for a single Chunk. Owned by a RoutingTableHistory.
 */
class ChunkInfo {
public:
    explicit ChunkInfo(const ChunkType& from);

    const auto& getRange() const {
        return _range;
    }

    const BSONObj& getMin() const {
        return _range.getMin();
    }

    const BSONObj& getMax() const {
        return _range.getMax();
    }

    const ShardId& getShardIdAt(const boost::optional<Timestamp>& ts) const;

    /**
     * Throws MigrationConflict if the history entry valid for the given timestamp is not the most
     * recent entry (meaning the chunk has moved).
     *
     * Throws StaleChunkHistory if no history entry is valid for the given timestamp.
     */
    void throwIfMovedSince(const Timestamp& ts) const;

    ChunkVersion getLastmod() const {
        return _lastmod;
    }

    const auto& getHistory() const {
        return _history;
    }

    bool isJumbo() const {
        return _jumbo;
    }

    /**
     * Get writes tracker for this chunk.
     */
    std::shared_ptr<ChunkWritesTracker> getWritesTracker() const {
        return _writesTracker;
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
     * Marks this chunk as jumbo. Only moves from false to true once and is used by the balancer.
     */
    void markAsJumbo();

private:
    const ChunkRange _range;

    const ShardId _shardId;

    const ChunkVersion _lastmod;

    const std::vector<ChunkHistory> _history;

    // Indicates whether this chunk should be treated as jumbo and not attempted to be moved or
    // split
    mutable bool _jumbo;

    // Used for tracking writes to this chunk, to estimate its size for the autosplitter. Since
    // ChunkInfo objects are always treated as const, and this contains metadata about the chunk
    // that needs to change, it's okay (and necessary) to mark it mutable.
    mutable std::shared_ptr<ChunkWritesTracker> _writesTracker;
};

class Chunk {
public:
    Chunk(ChunkInfo& chunkInfo, const boost::optional<Timestamp>& atClusterTime)
        : _chunkInfo(chunkInfo), _atClusterTime(atClusterTime) {}

    const BSONObj& getMin() const {
        return _chunkInfo.getMin();
    }

    const BSONObj& getMax() const {
        return _chunkInfo.getMax();
    }

    const ShardId& getShardId() const {
        return _chunkInfo.getShardIdAt(_atClusterTime);
    }

    /**
     * Throws MigrationConflict if the history entry valid for the chunk's pinned cluster time, if
     * it has one, is not the most recent entry (meaning the chunk has moved).
     *
     * Throws StaleChunkHistory if no history entry is valid for the chunk's cluster time.
     */
    void throwIfMoved() const;

    ChunkVersion getLastmod() const {
        return _chunkInfo.getLastmod();
    }

    const auto& getHistory() const {
        return _chunkInfo.getHistory();
    }

    bool isJumbo() const {
        return _chunkInfo.isJumbo();
    }

    /**
     * Get writes tracker for this chunk.
     */
    std::shared_ptr<ChunkWritesTracker> getWritesTracker() const {
        return _chunkInfo.getWritesTracker();
    }

    /**
     * Returns a string represenation of the chunk for logging.
     */
    std::string toString() const {
        return _chunkInfo.toString();
    }

    // Returns true if this chunk contains the given shard key, and false otherwise
    //
    // Note: this function takes an extracted *key*, not an original document (the point may be
    // computed by, say, hashing a given field or projecting to a subset of fields).
    bool containsKey(const BSONObj& shardKey) const {
        return _chunkInfo.containsKey(shardKey);
    }

    /**
     * Marks this chunk as jumbo. Only moves from false to true once and is used by the balancer.
     */
    void markAsJumbo() {
        _chunkInfo.markAsJumbo();
    }

private:
    ChunkInfo& _chunkInfo;
    const boost::optional<Timestamp> _atClusterTime;
};

}  // namespace mongo
