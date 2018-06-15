/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class ChunkWritesTracker {
public:
    /**
     * A factor that determines when a chunk should be split. We should split once data *
     * kSplitTestFactor > chunkSize (approximately).
     */
    static constexpr uint64_t kSplitTestFactor = 5;

    /**
     * Add more bytes written to the chunk.
     */
    void addBytesWritten(uint64_t bytesWritten) {
        _bytesWritten.fetchAndAdd(bytesWritten);
    }

    /**
     * Returns the total number of bytes that have been written to the chunk.
     */
    uint64_t getBytesWritten() {
        return _bytesWritten.loadRelaxed();
    }

    /**
     * Sets the number of bytes in the tracker to zero.
     */
    void clearBytesWritten();

    /**
     * Returns whether or not this chunk is ready to be split based on the
     * maximum allowable size of a chunk.
     */
    bool shouldSplit(uint64_t maxChunkSize);

    /**
     * Locks the chunk for splitting, returning false if it is already locked.
     * While it is locked, shouldSplit will always return false.
     */
    bool acquireSplitLock();

    /**
     * Releases the lock acquired for splitting.
     */
    void releaseSplitLock();

private:
    /**
     * The number of bytes that have been written to this chunk. May be
     * modified concurrently by several threads.
     */
    AtomicUInt64 _bytesWritten{0};

    /**
     * Protects _splitState when starting a split.
     */
    stdx::mutex _mtx;

    /**
     * Whether or not a current split is in progress for this chunk.
     */
    bool _isLockedForSplitting{false};
};

}  // namespace mongo
