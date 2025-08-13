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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/platform/atomic_word.h"

#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObj;

/**
 * Represents a cache entry for a single Chunk. Owned by a RoutingTableHistory.
 */
class ChunkInfo {
public:
    explicit ChunkInfo(const ChunkType& from);

    ChunkInfo(ChunkRange range,
              std::string maxKeyString,
              ShardId shardId,
              ChunkVersion version,
              std::vector<ChunkHistory> history,
              bool jumbo);

    const auto& getRange() const {
        return _range;
    }

    const BSONObj& getMin() const {
        return _range.getMin();
    }

    const BSONObj& getMax() const {
        return _range.getMax();
    }

    bool overlapsWith(const ChunkInfo& other) const {
        // Comparing keystrings is more performant than comparing BSONObj
        const auto minKeyString = ShardKeyPattern::toKeyString(getMin());
        return minKeyString < other.getMaxKeyString() &&
            getMaxKeyString() > ShardKeyPattern::toKeyString(other.getMin());
    }

    const std::string& getMaxKeyString() const {
        return _maxKeyString;
    }

    const ShardId& getShardId() const {
        return _shardId;
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
        return _jumbo.load();
    }

    /**
     * Returns a string represenation of the chunk for logging.
     */
    std::string toString() const;

    BSONObj toBSON() const;

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
    // IMPORTANT: The order of the members here mattters,
    // as it affects the performance of ChunkManager.
    // '_maxKeyString' must remain first member of this class because it is frequently
    // accessed by the ChunkManager.
    const std::string _maxKeyString;

    const ChunkRange _range;

    const ShardId _shardId;

    const ChunkVersion _lastmod;

    const std::vector<ChunkHistory> _history;

    // Indicates whether this chunk should be treated as jumbo and not attempted to be moved or
    // split
    AtomicWord<bool> _jumbo;
};

class Chunk {
public:
    Chunk(ChunkInfo& chunkInfo, const boost::optional<Timestamp>& atClusterTime)
        : _chunkInfo(chunkInfo), _atClusterTime(atClusterTime) {}

    Chunk(const Chunk& other) = default;

    const BSONObj& getMin() const {
        return _chunkInfo.getMin();
    }

    const BSONObj& getMax() const {
        return _chunkInfo.getMax();
    }

    const ShardId& getShardId() const {
        return _chunkInfo.getShardIdAt(_atClusterTime);
    }

    const auto& getRange() const {
        return _chunkInfo.getRange();
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
     * Returns a string represenation of the chunk for logging.
     */
    std::string toString() const;
    BSONObj toBSON() const;

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
