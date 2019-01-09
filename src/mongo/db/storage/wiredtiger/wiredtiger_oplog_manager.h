
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

class WiredTigerRecordStore;
class WiredTigerSessionCache;


// Manages oplog visibility, by periodically querying WiredTiger's all_committed timestamp value and
// then using that timestamp for all transactions that read the oplog collection.
class WiredTigerOplogManager {
    MONGO_DISALLOW_COPYING(WiredTigerOplogManager);

public:
    WiredTigerOplogManager() {}
    ~WiredTigerOplogManager() {}

    // This method will initialize the oplog read timestamp and start the background thread that
    // refreshes the value.
    void start(OperationContext* opCtx,
               const std::string& uri,
               WiredTigerRecordStore* oplogRecordStore);

    void halt();

    bool isRunning() {
        stdx::lock_guard<stdx::mutex> lk(_oplogVisibilityStateMutex);
        return _isRunning && !_shuttingDown;
    }

    // The oplogReadTimestamp is the timestamp used for oplog reads, to prevent readers from
    // reading past uncommitted transactions (which may create "holes" in the oplog after an
    // unclean shutdown).
    std::uint64_t getOplogReadTimestamp() const;
    void setOplogReadTimestamp(Timestamp ts);

    // Triggers the oplogJournal thread to update its oplog read timestamp, by flushing the journal.
    void triggerJournalFlush();

    // Waits until all committed writes at this point to become visible (that is, no holes exist in
    // the oplog.)
    void waitForAllEarlierOplogWritesToBeVisible(const WiredTigerRecordStore* oplogRecordStore,
                                                 OperationContext* opCtx);

    // Returns the all committed timestamp. All transactions with timestamps earlier than the
    // all committed timestamp are committed.
    uint64_t fetchAllCommittedValue(WT_CONNECTION* conn);

private:
    void _oplogJournalThreadLoop(WiredTigerSessionCache* sessionCache,
                                 WiredTigerRecordStore* oplogRecordStore);

    void _setOplogReadTimestamp(WithLock, uint64_t newTimestamp);

    stdx::thread _oplogJournalThread;
    mutable stdx::mutex _oplogVisibilityStateMutex;
    mutable stdx::condition_variable
        _opsWaitingForJournalCV;  // Signaled to trigger a journal flush.
    mutable stdx::condition_variable
        _opsBecameVisibleCV;  // Signaled when a journal flush is complete.

    bool _isRunning = false;             // Guarded by oplogVisibilityStateMutex.
    bool _shuttingDown = false;          // Guarded by oplogVisibilityStateMutex.
    bool _opsWaitingForJournal = false;  // Guarded by oplogVisibilityStateMutex.

    // When greater than 0, indicates that there are operations waiting for oplog visibility, and
    // journal flushing should not be delayed.
    std::int64_t _opsWaitingForVisibility = 0;  // Guarded by oplogVisibilityStateMutex.

    AtomicWord<unsigned long long> _oplogReadTimestamp;
};
}  // namespace mongo
