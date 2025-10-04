/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/storage/index_entry_comparison.h"

namespace mongo {

// We only need as many bits as the maximum number of components in a compound index (which is a
// fixed constant).
using ShardKeyMaskBitset = std::bitset<Ordering::kMaxCompoundIndexKeys>;

/**
 * This is a utility class that can be used when we encountar an orphan key during an index/distinct
 * scan, which allows us to construct an index seek point so we can seek to the next owned chunk
 * boundary, hence "skipping" an orphan chunk.
 */
class OrphanChunkSkipper {
public:
    /**
     * Helper struct which tracks which components in an index key pattern are shard key components.
     */
    struct ShardKeyMask {
        // Bits set to '1' are shard key components in the index key pattern.
        const ShardKeyMaskBitset bits;
        // Last shard key component position, i.e. position of last set bit in 'bits'.
        const int lastShardKeyPos;

        bool operator==(const OrphanChunkSkipper::ShardKeyMask& other) const = default;
    };

    /**
     * Factory function for chunk skipper creation. Takes a shard filterer, a 'shardKeyPattern', a
     * 'keyPattern' representing the index, and a scan direction for the index. Returns a chunk
     * skipper if it is possible to skip chunks for this combination of shard key & index, and
     * boost::none if not.
     */
    static boost::optional<OrphanChunkSkipper> tryMakeChunkSkipper(
        const ShardFilterer& shardFilterer,
        const ShardKeyPattern& shardKeyPattern,
        const BSONObj& keyPattern,
        int scanDirection);

    OrphanChunkSkipper(OrphanChunkSkipper&&) = default;

    /**
     * See makeSeekPointIfOrphan() below for what each enum value means.
     */
    enum Info { NotOrphan, CanSkipOrphans, NoMoreOwnedForThisPrefix, NoMoreOwned };

    /**
     * This function takes a 'curIxKeyValue' and returns an Action to the caller depending on
     * whether this key is an orphan, and whether or not there are anymore owned chunks in the
     * current scan direction.
     *  - If 'curIxKeyValue' is not an orphan, we return Action::NotOrphan.
     *  - Else, if there is an owned shard key value in the current scan direction, we update
     * 'ixSeekPtOut' to point to this value, and return Action::CanSkipOrphans. For example, say we
     * are scanning index index {a: 1, b: 1}, we have shard key {b: 1}, and our 'curIxKeyValue' is
     * an orphan {a: 10, b: 10}. If our next owned chunk starts at {"b": 20}, we would update the
     * seek point to inclusively seek to the next entry with the same non-shard-key prefix, that is
     * to say: {"":  1, "": 20}. See the implementation of _makeSeekPointForChunkBoundary() for
     * details on how we set the seek point.
     *  - Else, if there are no further owned shard key values in the current scan direction, we
     * return Action::NoMoreOwnedForThisPrefix if the shard key is not a contiguous prefix of the
     * index key pattern; otherwise we return Action::NoMoreOwned, because there can not be any more
     * owned values in this collection.
     */
    Info makeSeekPointIfOrphan(const BSONObj& curIxKeyValue, IndexSeekPoint& ixSeekPtOut);

    /**
     * Same as above, only takes an IndexKeyDatum rather than a BSONObj.
     */
    Info makeSeekPointIfOrphan(const std::vector<IndexKeyDatum>& keyData,
                               IndexSeekPoint& ixSeekPtOut);

    const ShardKeyMask& getShardKeyMask() const {
        return _shardKeyMask;
    }

    ChunkMap::Direction getChunkMapScanDir() const {
        return _chunkMapScanDir;
    }

private:
    OrphanChunkSkipper(const ShardFilterer& shardFilterer,
                       ShardKeyMask&& shardKeyMask,
                       ChunkMap::Direction chunkMapScanDir)
        : _shardFilterer(shardFilterer),
          _shardKeyMask(std::move(shardKeyMask)),
          _chunkMapScanDir(chunkMapScanDir) {}

    // Helper function to implement the logic for updating a seek point to point to the next owned
    // chunk boundary.
    void _makeSeekPointForChunkBoundary(const BSONObj& nextChunkBoundary,
                                        const BSONObj& currentKey,
                                        IndexSeekPoint& ixSeekPtOut) const;

    // Helper function to extract the value at the shard key from a BSON representing the current
    // entry in the index + field names. Returns an unowned BSONObj (i.e. a temporary one).
    BSONObj _extractShardKey(const BSONObj& keyData);

    // Information we use to determine what the next owned chunk boundary is (if any).
    const ShardFilterer& _shardFilterer;
    const ShardKeyMask _shardKeyMask;
    // Direction we use to scan the chunk map in order to get the next owned chunk.
    const ChunkMap::Direction _chunkMapScanDir;

    // Used to extract a temporary BSON representing the shard key.
    BufBuilder _bufForShardKey;
    // Used to extract a temporary BSON representing the index key from an IndexKeyDatum.
    BufBuilder _bufForIndexKey;
};

}  // namespace mongo
