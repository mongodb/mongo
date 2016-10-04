/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include <boost/optional.hpp>

#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class OperationContext;
class RecordId;

// Keep "milestones" against the oplog to efficiently remove the old records when the collection
// grows beyond its desired maximum size.
class WiredTigerRecordStore::OplogStones {
public:
    struct Stone {
        int64_t records;      // Approximate number of records in a chunk of the oplog.
        int64_t bytes;        // Approximate size of records in a chunk of the oplog.
        RecordId lastRecord;  // RecordId of the last record in a chunk of the oplog.
    };

    OplogStones(OperationContext* txn, WiredTigerRecordStore* rs);

    bool isDead();

    void kill();

    bool hasExcessStones() const {
        return _stones.size() > _numStonesToKeep;
    }

    void awaitHasExcessStonesOrDead();

    boost::optional<OplogStones::Stone> peekOldestStoneIfNeeded() const;

    void popOldestStone();

    void createNewStoneIfNeeded(RecordId lastRecord);

    void updateCurrentStoneAfterInsertOnCommit(OperationContext* txn,
                                               int64_t bytesInserted,
                                               RecordId highestInserted,
                                               int64_t countInserted);

    void clearStonesOnCommit(OperationContext* txn);

    // Updates the metadata about the oplog stones after a rollback occurs.
    void updateStonesAfterCappedTruncateAfter(int64_t recordsRemoved,
                                              int64_t bytesRemoved,
                                              RecordId firstRemovedId);

    // The start point of where to truncate next. Used by the background reclaim thread to
    // efficiently truncate records with WiredTiger by skipping over tombstones, etc.
    RecordId firstRecord;

    //
    // The following methods are public only for use in tests.
    //

    size_t numStones() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _stones.size();
    }

    int64_t currentBytes() const {
        return _currentBytes.load();
    }

    int64_t currentRecords() const {
        return _currentRecords.load();
    }

    void setMinBytesPerStone(int64_t size);

    void setNumStonesToKeep(size_t numStones);

private:
    class InsertChange;
    class TruncateChange;

    void _calculateStones(OperationContext* txn);
    void _calculateStonesByScanning(OperationContext* txn);
    void _calculateStonesBySampling(OperationContext* txn,
                                    int64_t estRecordsPerStone,
                                    int64_t estBytesPerStone);

    void _pokeReclaimThreadIfNeeded();

    static const uint64_t kRandomSamplesPerStone = 10;

    WiredTigerRecordStore* _rs;

    stdx::mutex _oplogReclaimMutex;
    stdx::condition_variable _oplogReclaimCv;

    // True if '_rs' has been destroyed, e.g. due to repairDatabase being called on the "local"
    // database, and false otherwise.
    bool _isDead = false;

    // Maximum number of stones to keep in the deque before the background reclaim thread should
    // truncate the oldest ones. Does not include the stone currently being filled. This value
    // should not be changed after initialization.
    size_t _numStonesToKeep;
    // Minimum number of bytes the stone being filled should contain before it gets added to the
    // deque of oplog stones. This value should not be changed after initialization.
    int64_t _minBytesPerStone;

    AtomicInt64 _currentRecords;  // Number of records in the stone being filled.
    AtomicInt64 _currentBytes;    // Number of bytes in the stone being filled.

    mutable stdx::mutex _mutex;  // Protects against concurrent access to the deque of oplog stones.
    std::deque<OplogStones::Stone> _stones;  // front = oldest, back = newest.
};

}  // namespace mongo
