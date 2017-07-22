/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/chunk_size_tracker.h"

#include "mongo/bson/simple_bsonobj_comparator.h"

namespace mongo {

ChunkSizeStats::ChunkSizeStats(const BSONObj& maxKey, uint64_t sizeInBytes)
    : _maxKey(maxKey), _sizeInBytes(sizeInBytes) {}

ChunkSizeStats::~ChunkSizeStats() {}

ChunkSizeTracker::ChunkSizeTracker()
    : _chunkSizeMap(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<ChunkSizeStats>()) {}

ChunkSizeTracker::~ChunkSizeTracker() {}

bool ChunkSizeTracker::noteBytes(const BSONObj& minChunkKey,
                                 const BSONObj& maxChunkKey,
                                 uint64_t deltaInBytes,
                                 uint64_t maxChunkSize) {

    // TODO: consider overlapping chunk boundaries within the minChunkKey
    //      e.g. [10, 20]
    //           [15, 20]

    // Find the max key and size of the chunk, using the min key as an index. If such
    // a chunk does not exist in the map, create a new entry in the map.
    auto it = _chunkSizeMap.find(minChunkKey);
    if (it == _chunkSizeMap.end()) {
        it = _chunkSizeMap.emplace(minChunkKey, ChunkSizeStats(maxChunkKey, deltaInBytes)).first;
    } else {
        // If the max key matches, then increment the tracked size of that chunk.
        // Otherwise, the max key is invalid, and we throw an error.
        if (it->second.getMaxKey().woCompare(maxChunkKey) == 0) {
            it->second.increment(deltaInBytes);
        } else {
            uasserted(40531, "specified max chunk key does not match actual max chunk key");
        }
    }

    // The split threshold will be 20% of the maximum chunk size. If the current
    // chunk size exceeds this threshold, then we should split.
    uint64_t splitThreshold = maxChunkSize / 5;
    if (it->second.getSizeInBytes() > splitThreshold) {
        it->second.reset();
        return true;
    }
    return false;
}

void ChunkSizeTracker::forgetRange(const BSONObj& minChunkKey, const BSONObj& maxChunkKey) {
    auto it = _chunkSizeMap.find(minChunkKey);
    if (it != _chunkSizeMap.end() && it->second.getMaxKey().woCompare(maxChunkKey) == 0) {
        _chunkSizeMap.erase(minChunkKey);
    }
}

}  // namespace mongo
