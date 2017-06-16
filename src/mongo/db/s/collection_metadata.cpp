/**
 *    Copyright (C) 2012 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/collection_metadata.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/builder.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

CollectionMetadata::CollectionMetadata(const BSONObj& keyPattern,
                                       ChunkVersion collectionVersion,
                                       ChunkVersion shardVersion,
                                       RangeMap shardChunksMap)
    : _shardKeyPattern(keyPattern),
      _collVersion(collectionVersion),
      _shardVersion(shardVersion),
      _chunksMap(std::move(shardChunksMap)),
      _rangesMap(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<CachedChunkInfo>()) {

    invariant(_shardKeyPattern.isValid());
    invariant(_collVersion.epoch() == _shardVersion.epoch());
    invariant(_collVersion.isSet());
    invariant(_collVersion >= _shardVersion);

    if (_chunksMap.empty()) {
        invariant(!_shardVersion.isSet());
        return;
    }
    invariant(_shardVersion.isSet());

    _buildRangesMap();
}

CollectionMetadata::~CollectionMetadata() = default;

void CollectionMetadata::_buildRangesMap() {
    _rangesMap.clear();

    // Load the chunk information, coalescing their ranges. The version for this shard would be
    // the highest version for any of the chunks.

    BSONObj min, max;

    for (const auto& entry : _chunksMap) {
        BSONObj const& currMin = entry.first;
        BSONObj const& currMax = entry.second.getMaxKey();

        // Coalesce the chunk's bounds in ranges if they are adjacent chunks
        if (min.isEmpty()) {
            min = currMin;
            max = currMax;
            continue;
        }

        if (SimpleBSONObjComparator::kInstance.evaluate(max == currMin)) {
            max = currMax;
            continue;
        }

        _rangesMap.emplace(min, CachedChunkInfo(max, ChunkVersion::IGNORED()));

        min = currMin;
        max = currMax;
    }

    invariant(!min.isEmpty());
    invariant(!max.isEmpty());

    _rangesMap.emplace(min, CachedChunkInfo(max, ChunkVersion::IGNORED()));
}

std::unique_ptr<CollectionMetadata> CollectionMetadata::clone() const {
    return stdx::make_unique<CollectionMetadata>(
        _shardKeyPattern.toBSON(), getCollVersion(), getShardVersion(), getChunks());
}

bool CollectionMetadata::keyBelongsToMe(const BSONObj& key) const {
    if (_rangesMap.empty()) {
        return false;
    }

    auto it = _rangesMap.upper_bound(key);
    if (it != _rangesMap.begin())
        it--;

    return rangeContains(it->first, it->second.getMaxKey(), key);
}

bool CollectionMetadata::getNextChunk(const BSONObj& lookupKey, ChunkType* chunk) const {
    RangeMap::const_iterator upperChunkIt = _chunksMap.upper_bound(lookupKey);
    RangeMap::const_iterator lowerChunkIt = upperChunkIt;

    if (upperChunkIt != _chunksMap.begin()) {
        --lowerChunkIt;
    } else {
        lowerChunkIt = _chunksMap.end();
    }

    if (lowerChunkIt != _chunksMap.end() &&
        lowerChunkIt->second.getMaxKey().woCompare(lookupKey) > 0) {
        chunk->setMin(lowerChunkIt->first);
        chunk->setMax(lowerChunkIt->second.getMaxKey());
        chunk->setVersion(lowerChunkIt->second.getVersion());
        return true;
    }

    if (upperChunkIt != _chunksMap.end()) {
        chunk->setMin(upperChunkIt->first);
        chunk->setMax(upperChunkIt->second.getMaxKey());
        chunk->setVersion(upperChunkIt->second.getVersion());
        return true;
    }

    return false;
}

bool CollectionMetadata::getDifferentChunk(const BSONObj& chunkMinKey,
                                           ChunkType* differentChunk) const {
    RangeMap::const_iterator upperChunkIt = _chunksMap.end();
    RangeMap::const_iterator lowerChunkIt = _chunksMap.begin();

    while (lowerChunkIt != upperChunkIt) {
        if (lowerChunkIt->first.woCompare(chunkMinKey) != 0) {
            differentChunk->setMin(lowerChunkIt->first);
            differentChunk->setMax(lowerChunkIt->second.getMaxKey());
            differentChunk->setVersion(lowerChunkIt->second.getVersion());
            return true;
        }
        ++lowerChunkIt;
    }

    return false;
}

Status CollectionMetadata::checkChunkIsValid(const ChunkType& chunk) {
    ChunkType existingChunk;

    if (!getNextChunk(chunk.getMin(), &existingChunk)) {
        return {ErrorCodes::StaleShardVersion,
                str::stream() << "Chunk with bounds "
                              << ChunkRange(chunk.getMin(), chunk.getMax()).toString()
                              << " is not owned by this shard."};
    }

    if (existingChunk.getMin().woCompare(chunk.getMin()) ||
        existingChunk.getMax().woCompare(chunk.getMax())) {
        return {ErrorCodes::StaleShardVersion,
                str::stream() << "Unable to find chunk with the exact bounds "
                              << ChunkRange(chunk.getMin(), chunk.getMax()).toString()
                              << " at collection version "
                              << getCollVersion().toString()};
    }

    return Status::OK();
}

bool CollectionMetadata::rangeOverlapsChunk(ChunkRange const& range) {
    return rangeMapOverlaps(_rangesMap, range.getMin(), range.getMax());
}

void CollectionMetadata::toBSONBasic(BSONObjBuilder& bb) const {
    _collVersion.addToBSON(bb, "collVersion");
    _shardVersion.addToBSON(bb, "shardVersion");
    bb.append("keyPattern", _shardKeyPattern.toBSON());
}

void CollectionMetadata::toBSONChunks(BSONArrayBuilder& bb) const {
    if (_chunksMap.empty())
        return;

    for (RangeMap::const_iterator it = _chunksMap.begin(); it != _chunksMap.end(); ++it) {
        BSONArrayBuilder chunkBB(bb.subarrayStart());
        chunkBB.append(it->first);
        chunkBB.append(it->second.getMaxKey());
        chunkBB.done();
    }
}

std::string CollectionMetadata::toStringBasic() const {
    return str::stream() << "collection version: " << _collVersion.toString()
                         << ", shard version: " << _shardVersion.toString();
}

boost::optional<KeyRange> CollectionMetadata::getNextOrphanRange(
    RangeMap const& receivingChunks, BSONObj const& origLookupKey) const {

    BSONObj lookupKey = origLookupKey;
    BSONObj maxKey = getMaxKey();  // so we don't keep rebuilding
    while (lookupKey.woCompare(maxKey) < 0) {

        using Its = std::pair<RangeMap::const_iterator, RangeMap::const_iterator>;

        auto patchLookupKey = [&](RangeMap const& map) -> boost::optional<Its> {
            auto lowerIt = map.end(), upperIt = map.end();

            if (!map.empty()) {
                upperIt = map.upper_bound(lookupKey);
                lowerIt = upperIt;
                if (upperIt != map.begin())
                    --lowerIt;
                else
                    lowerIt = map.end();
            }

            // If we overlap, continue after the overlap
            // TODO: Could optimize slightly by finding next non-contiguous chunk
            if (lowerIt != map.end() && lowerIt->second.getMaxKey().woCompare(lookupKey) > 0) {
                lookupKey = lowerIt->second.getMaxKey();  // note side effect
                return boost::none;
            } else {
                return Its(lowerIt, upperIt);
            }
        };

        boost::optional<Its> chunksIts, pendingIts;
        if (!(chunksIts = patchLookupKey(_chunksMap)) ||
            !(pendingIts = patchLookupKey(receivingChunks))) {
            continue;
        }

        boost::optional<KeyRange> range =
            KeyRange("", getMinKey(), maxKey, _shardKeyPattern.toBSON());

        auto patchArgRange = [&range](RangeMap const& map, Its its) {
            // We know that the lookup key is not covered by a chunk or pending range, and where the
            // previous chunk and pending chunks are.  Now we fill in the bounds as the closest
            // bounds of the surrounding ranges in both maps.
            auto lowerIt = its.first, upperIt = its.second;

            if (lowerIt != map.end() && lowerIt->second.getMaxKey().woCompare(range->minKey) > 0) {
                range->minKey = lowerIt->second.getMaxKey();
            }
            if (upperIt != map.end() && upperIt->first.woCompare(range->maxKey) < 0) {
                range->maxKey = upperIt->first;
            }
        };

        patchArgRange(_chunksMap, *chunksIts);
        patchArgRange(receivingChunks, *pendingIts);
        return range;
    }

    return boost::none;
}

BSONObj CollectionMetadata::getMinKey() const {
    return _shardKeyPattern.getKeyPattern().globalMin();
}

BSONObj CollectionMetadata::getMaxKey() const {
    return _shardKeyPattern.getKeyPattern().globalMax();
}

bool CollectionMetadata::isValidKey(const BSONObj& key) const {
    return _shardKeyPattern.isShardKey(key);
}

}  // namespace mongo
