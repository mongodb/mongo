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

#include <boost/optional.hpp>

#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"

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
        Date_t wallTime;      // Walltime of when this chunk of the oplog was created.

        Stone(int64_t records, int64_t bytes, RecordId lastRecord, Date_t wallTime)
            : records(records),
              bytes(bytes),
              lastRecord(std::move(lastRecord)),
              wallTime(wallTime) {}
    };

    OplogStones(OperationContext* opCtx, WiredTigerRecordStore* rs);

    bool isDead();

    void kill();

    bool hasExcessStones_inlock() const;

    void awaitHasExcessStonesOrDead();

    void getOplogStonesStats(BSONObjBuilder& builder) const {
        builder.append("totalTimeProcessingMicros", _totalTimeProcessing.load());
        builder.append("processingMethod", _processBySampling.load() ? "sampling" : "scanning");
        if (auto oplogMinRetentionHours = storageGlobalParams.oplogMinRetentionHours.load()) {
            builder.append("oplogMinRetentionHours", oplogMinRetentionHours);
        }
    }

    boost::optional<OplogStones::Stone> peekOldestStoneIfNeeded() const;

    void popOldestStone();

    void createNewStoneIfNeeded(OperationContext* opCtx,
                                const RecordId& lastRecord,
                                Date_t wallTime);

    void updateCurrentStoneAfterInsertOnCommit(OperationContext* opCtx,
                                               int64_t bytesInserted,
                                               const Record& highestInsertedRecord,
                                               int64_t countInserted);

    void clearStonesOnCommit(OperationContext* opCtx);

    // Updates the metadata about the oplog stones after a rollback occurs.
    void updateStonesAfterCappedTruncateAfter(int64_t recordsRemoved,
                                              int64_t bytesRemoved,
                                              const RecordId& firstRemovedId);

    // Resize oplog size
    void adjust(int64_t maxSize);

    // The start point of where to truncate next. Used by the background reclaim thread to
    // efficiently truncate records with WiredTiger by skipping over tombstones, etc.
    RecordId firstRecord;

    //
    // The following methods are public only for use in tests.
    //

    size_t numStones() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return _stones.size();
    }

    int64_t currentBytes() const {
        return _currentBytes.load();
    }

    int64_t currentRecords() const {
        return _currentRecords.load();
    }

    void setMinBytesPerStone(int64_t size);

    bool processedBySampling() const {
        return _processBySampling.load();
    }

private:
    class InsertChange;

    void _calculateStones(OperationContext* opCtx, size_t size);
    void _calculateStonesByScanning(OperationContext* opCtx);
    void _calculateStonesBySampling(OperationContext* opCtx,
                                    int64_t estRecordsPerStone,
                                    int64_t estBytesPerStone);

    void _pokeReclaimThreadIfNeeded();

    static const uint64_t kRandomSamplesPerStone = 10;

    WiredTigerRecordStore* _rs;

    Mutex _oplogReclaimMutex;
    stdx::condition_variable _oplogReclaimCv;

    // True if '_rs' has been destroyed, e.g. due to repairDatabase being called on the "local"
    // database, and false otherwise.
    bool _isDead = false;

    // Minimum number of bytes the stone being filled should contain before it gets added to the
    // deque of oplog stones.
    int64_t _minBytesPerStone;

    AtomicWord<long long> _currentRecords;     // Number of records in the stone being filled.
    AtomicWord<long long> _currentBytes;       // Number of bytes in the stone being filled.
    AtomicWord<int64_t> _totalTimeProcessing;  // Amount of time spent scanning and/or sampling the
                                               // oplog during start up, if any.
    AtomicWord<bool> _processBySampling;       // Whether the oplog was sampled or scanned.

    // Protects against concurrent access to the deque of oplog stones.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("OplogStones::_mutex");
    std::deque<OplogStones::Stone> _stones;  // front = oldest, back = newest.
};

}  // namespace mongo
