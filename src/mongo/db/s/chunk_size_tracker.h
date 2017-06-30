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

#pragma once

#include <memory>

#include "mongo/bson/bsonobj_comparator_interface.h"

namespace mongo {

/**
 * Class representing the max key of a chunk along with that chunk's size in
 * bytes.
 */
class ChunkSizeStats {
public:
    /**
     * Constructor for ChunkSizeStats class.
     */
    ChunkSizeStats(const BSONObj& maxKey, uint64_t sizeInBytes = 0);

    ~ChunkSizeStats();

    /**
     * Returns the max key associated with the chunk.
     */
    const BSONObj& getMaxKey() const {
        return _maxKey;
    }

    /**
     * Returns the size in bytes of the chunk.
     */
    uint64_t getSizeInBytes() const {
        return _sizeInBytes;
    };

    /**
     * Increases the size in bytes of the chunk by a delta parameter.
     */
    void increment(uint64_t deltaInBytes) {
        _sizeInBytes += deltaInBytes;
    }

    /**
     * Resets the size in bytes of the chunk to 0.
     */
    void reset() {
        _sizeInBytes = 0;
    }

private:
    BSONObj _maxKey;
    uint64_t _sizeInBytes;
};

// Map from min key to a ChunkSizeStats which contains the max key and the size
// of the chunk
using ChunkSizeMap = BSONObjIndexedMap<ChunkSizeStats>;

class ChunkSizeTracker {
public:
    ChunkSizeTracker();
    ~ChunkSizeTracker();

    /**
     * Returns true if the range should potentially be split.
     */
    bool noteBytes(const BSONObj& minChunkKey,
                   const BSONObj& maxChunkKey,
                   uint64_t deltaInBytes,
                   uint64_t maxChunkSize);

    /**
     * Discontinue tracking the given range.
     */
    void forgetRange(const BSONObj& minChunkKey, const BSONObj& maxChunkKey);

    /**
     * Return the ChunkSizeMap.
     */
    const ChunkSizeMap& getChunkSizeMap() const {
        return _chunkSizeMap;
    }

private:
    ChunkSizeMap _chunkSizeMap;
};

}  // namespace mongo
