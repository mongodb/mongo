// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObj;

/**
 * Represents a cache entry for a single Chunk. Owned by a RoutingTableHistory.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ChunkInfo {
public:
    explicit ChunkInfo(const ChunkType& from);

    ChunkInfo(ChunkRange range,
              std::string maxKeyString,
              std::string minKeyString,
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

    const std::string& getMinKeyString() const {
        return _minKeyString;
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
     * Returns a string representation of the chunk for logging.
     */
    std::string toString() const;

    BSONObj toBSON() const;

    // Returns true if this chunk contains the given shard key, and false otherwise
    //
    // Note: this function takes an extracted *key*, not an original document (the point may be
    // computed by, say, hashing a given field or projecting to a subset of fields).
    bool containsKey(const BSONObj& shardKey) const;

    void setJumbo(bool jumbo);

private:
    // IMPORTANT: The order of the members here matters,
    // as it affects the performance of ChunkManager.
    // '_maxKeyString' must remain first member of this class because it is frequently
    // accessed by the ChunkManager.
    const std::string _maxKeyString;

    const std::string _minKeyString;

    const ChunkRange _range;

    const ShardId _shardId;

    const ChunkVersion _lastmod;

    const std::vector<ChunkHistory> _history;

    // Indicates whether this chunk should be treated as jumbo and not attempted to be moved or
    // split
    Atomic<bool> _jumbo;
};

class [[MONGO_MOD_NEEDS_REPLACEMENT]] Chunk {
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
     * Returns a string representation of the chunk for logging.
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

    void setJumbo(bool jumbo) {
        _chunkInfo.setJumbo(jumbo);
    }

private:
    ChunkInfo& _chunkInfo;
    const boost::optional<Timestamp> _atClusterTime;
};

}  // namespace mongo
