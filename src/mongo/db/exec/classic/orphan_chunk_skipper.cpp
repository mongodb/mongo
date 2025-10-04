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

#include "mongo/db/exec/classic/orphan_chunk_skipper.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/util/assert_util.h"

#include <boost/optional/optional.hpp>
namespace mongo {
namespace {
/**
 * Helper which determines if we can apply this optimization. This maps the `shardKeyPattern` fields
 * in the input 'keyPattern' by setting bits corresponding to fields in the 'keyPattern' to 1 if
 * they belong to the 'shardKey', and 0 if not. Returns boost::none if:
 *  - Any fields in the shard key are not present in the index key pattern.
 *  - Shard key fields don't occur in the same order.
 *  - Shard key fields are not sorted in either the same or exactly opposite way as the index.
 *  - The hashed-ness of any shard key field differs to that of the 'keyPattern'.
 *  - The shardKey is compound and its fields are not contiguous within the index 'keyPattern'.
 *
 * Examples:
 *   keyPattern = {a: 1, b: 1, c: 1}
 *   shardKey = {b: 1, a: 1}
 *   scanDirection = 1 => boost::none
 *
 *   keyPattern = {a: 1, b: 1, c: 1}
 *   shardKey = {a: 1, b: 1}
 *   scanDirection = 1 => {1, 1, 0}
 *
 *   keyPattern = {a: 1, b: 1, c: 1, d: 1, e: 1}
 *   shardKey = {b: 1, c: 1, d: 1}
 *   scanDirection = 1 => {0, 1, 1, 1, 0}
 *
 *   keyPattern = {a: 1, b: 1, c: 1}
 *   shardKey = {a: 1, c: 1}
 *   scanDirection = 1 => boost::none
 *
 */
boost::optional<std::pair<OrphanChunkSkipper::ShardKeyMask, int>> extractShardKeyMaskAndScanDir(
    const ShardKeyPattern& shardKeyPattern, const BSONObj& keyPattern, int scanDirection) {
    ShardKeyMaskBitset shardKeyMask;
    int lastShardKeyPos = -1;

    const auto& shardKey = shardKeyPattern.toBSON();
    auto shardKeyIt = shardKey.begin();
    auto keyPatternIt = keyPattern.begin();
    int bitIdx = 0;
    int skDir = 0;

    while (shardKeyIt != shardKey.end() && keyPatternIt != keyPattern.end()) {
        if (shardKeyIt->fieldNameStringData() == keyPatternIt->fieldNameStringData()) {
            if (!shardKeyIt->isNumber() || !keyPatternIt->isNumber()) {
                // If our scan direction along the index doesn't match the shard key
                // direction then we should bail out here.
                // TODO SERVER-93562: Support for hashed indexes.
                break;
            } else if (lastShardKeyPos >= 0 && lastShardKeyPos < bitIdx - 1) {
                // The previous bit was unset (i.e. not a shard key component), but we had
                // previously seen a shard key component. The shard key is not contiguous in our
                // index spec, therefore we should bail out here.
                break;
            }

            lastShardKeyPos = bitIdx;

            auto dir = shardKeyIt->numberInt() * keyPatternIt->numberInt() * scanDirection;
            if (skDir == 0) {
                skDir = dir;
            } else if (skDir != dir) {
                // This key points in the "opposite" direction of previous shard keys. We can't
                // apply the optimization. Example: {a: 1, b: -1}, {a: -1, b: -1}, 1
                //  field "a": set skDir = 1 * -1 * 1 = -1
                //  field "b": dir = -1 * -1 * 1 != skDir.
                //  So we bail out.
                break;
            }

            shardKeyIt++;
            shardKeyMask[bitIdx].flip();
        }
        bitIdx++;
        keyPatternIt++;
    }

    // If we've hit the end of the shard key iterator, we've found all shard key components in
    // order in this index.
    if (shardKeyIt == shardKey.end()) {
        return boost::make_optional<std::pair<OrphanChunkSkipper::ShardKeyMask, int>>(
            {{shardKeyMask, lastShardKeyPos}, skDir});
    }

    return boost::none;
}
}  // namespace

boost::optional<OrphanChunkSkipper> OrphanChunkSkipper::tryMakeChunkSkipper(
    const ShardFilterer& shardFilterer,
    const ShardKeyPattern& shardKeyPattern,
    const BSONObj& keyPattern,
    int scanDirection) {
    if (auto extracted = extractShardKeyMaskAndScanDir(shardKeyPattern, keyPattern, scanDirection);
        extracted) {
        auto&& [shardKeyMask, chunkMapScanDir] = *extracted;
        return OrphanChunkSkipper(shardFilterer,
                                  std::move(shardKeyMask),
                                  chunkMapScanDir > 0 ? ChunkMap::Direction::Forward
                                                      : ChunkMap::Direction::Backward);
    }

    return boost::none;
}

BSONObj OrphanChunkSkipper::_extractShardKey(const BSONObj& ixkeyData) {
    _bufForShardKey.reset();
    BSONObjBuilder bob(_bufForShardKey);
    auto it = ixkeyData.begin();
    size_t i = 0;
    while (it != ixkeyData.end()) {
        if (_shardKeyMask.bits[i])
            bob.append(*it);
        it++;
        i++;
    }
    return bob.done();
}

void OrphanChunkSkipper::_makeSeekPointForChunkBoundary(const BSONObj& nextChunkBoundary,
                                                        const BSONObj& currentKey,
                                                        IndexSeekPoint& ixSeekPtOut) const {
    int i = 0;
    auto chunkIt = nextChunkBoundary.begin();
    auto currentKeyIt = currentKey.begin();

    // Build prefix. We will keep the first portion of the index pattern up to and excluding the
    // first shard key component whose value differs from the chunk boundary as part of the prefix.
    // We stop at the last shard key component (note: we should only reach that component and not
    // see a difference if our chunk boundary is exclusive).
    ixSeekPtOut.keyPrefix = currentKey.copy();
    ixSeekPtOut.prefixLen = 0;
    ixSeekPtOut.keySuffix = {};
    while (i <= _shardKeyMask.lastShardKeyPos) {
        if (!_shardKeyMask.bits[i] || currentKeyIt->binaryEqualValues(*chunkIt)) {
            // Include current key in prefix unchanged.
            ixSeekPtOut.prefixLen++;
            ixSeekPtOut.keySuffix.push_back(*currentKeyIt);
        } else {
            // Found first differing shard-key field.
            break;
        }
        if (_shardKeyMask.bits[i]) {
            ++chunkIt;
        }
        ++currentKeyIt;
        ++i;
    }

    // Build suffix. We will update all consecutive shard keys including and after the first
    // "differing" shard key (i.e. stop as soon as we see a non-shard-key component).
    while (i <= _shardKeyMask.lastShardKeyPos && _shardKeyMask.bits[i]) {
        // Copy over chunk boundary value for this component to suffix.
        ixSeekPtOut.keySuffix.push_back(*chunkIt);
        ++chunkIt;
        ++currentKeyIt;
        ++i;
    }

    // Seek exclusively if our chunk boundaries are exclusive and we are consuming the entire chunk
    // boundary. Otherwise seek inclusively. This is because we want to seek to exactly  the chunk
    // boundary if its inclusive (not skip over it) or when using only a subset of the fields of the
    // chunk boundary, and skip past it if it is exclusive. This will also ensure that if we don't
    // find a difference (because we landed on an orphan that is an exact match for a chunk's
    // exclusive upper bound), we will use the orphan key logically truncated to the last field that
    // is part of the shard key and seek exclusively there.
    if (_chunkMapScanDir == ChunkMap::Direction::Backward && i >= _shardKeyMask.lastShardKeyPos) {
        ixSeekPtOut.firstExclusive = _shardKeyMask.lastShardKeyPos;
    } else {
        ixSeekPtOut.firstExclusive = -1;
    }
}

OrphanChunkSkipper::Info OrphanChunkSkipper::makeSeekPointIfOrphan(
    const std::vector<IndexKeyDatum>& keyData, IndexSeekPoint& ixSeekPtOut) {
    tassert(9246500, "Expected a single index key datum", keyData.size() == 1);
    auto ikpIt = keyData[0].indexKeyPattern.begin();
    auto dataIt = keyData[0].keyData.begin();
    _bufForIndexKey.reset();
    BSONObjBuilder bob(_bufForIndexKey);
    while (ikpIt != keyData[0].indexKeyPattern.end()) {
        bob.appendAs(*dataIt, ikpIt->fieldNameStringData());
        tassert(9246501,
                "Expected keyData and indexKeyPattern to have the same length",
                dataIt != keyData[0].keyData.end());
        ikpIt++;
        dataIt++;
    }
    return makeSeekPointIfOrphan(bob.done(), ixSeekPtOut);
}

OrphanChunkSkipper::Info OrphanChunkSkipper::makeSeekPointIfOrphan(const BSONObj& curIxKeyValue,
                                                                   IndexSeekPoint& ixSeekPtOut) {
    auto chunkOwnership =
        _shardFilterer.nearestOwnedChunk(_extractShardKey(curIxKeyValue), _chunkMapScanDir);

    if (chunkOwnership.containsShardKey) {
        // This key is not an orphan.
        return NotOrphan;
    }

    if (!chunkOwnership.nearestOwnedChunk) {
        // This means we have run out of owned keys in the current scan direction for the chunk map.
        if (_shardKeyMask.bits[0]) {
            // Our shard key is a prefix of the index (as it must be contiguous for this class to
            // have been initialized). We can therefore end the scan now, as there can be no further
            // orphans.
            return NoMoreOwned;
        }

        // Our shard key is not a prefix, therefore even though we have exhausted the owned shard
        // key values, we may have more owned documents with a different index key pattern prefix.
        // The caller must decide how to proceed in this case.
        return NoMoreOwnedForThisPrefix;
    }

    // We have an orphan and we have found the nearest owned chunk boundary. Update the seek point
    // and let the caller know.
    _makeSeekPointForChunkBoundary(_chunkMapScanDir == ChunkMap::Direction::Forward
                                       ? chunkOwnership.nearestOwnedChunk->getMin()
                                       : chunkOwnership.nearestOwnedChunk->getMax(),
                                   curIxKeyValue,
                                   ixSeekPtOut);
    return CanSkipOrphans;
}


}  // namespace mongo
