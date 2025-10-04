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

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/slotted_timestamp_list.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace repl {

/**
 * The OplogVisibilityManager manages oplog visibility by tracking timestamps that transactions
 * reserve.
 */
class OplogVisibilityManager {
public:
    using iterator = SlottedTimestampList::iterator;
    using const_iterator = SlottedTimestampList::const_iterator;

    RecordStore* getRecordStore() const;

    OplogVisibilityManager() = default;
    OplogVisibilityManager(const OplogVisibilityManager& rhs) = delete;
    OplogVisibilityManager& operator=(const OplogVisibilityManager& rhs) = delete;
    OplogVisibilityManager(const OplogVisibilityManager&& rhs) = delete;
    OplogVisibilityManager& operator=(const OplogVisibilityManager&& rhs) = delete;

    /**
     * Re-initializes the oplog visibility manager with the given record store and initial
     * timestamp. Called by LocalOplogInfo when the oplog record store pointer is set.
     */
    void reInit(RecordStore* rs, const Timestamp& initialTs);

    /**
     * Clears the oplog visibility manager.
     */
    void clear();

    /**
     * Start tracking the timestamps given the first and last timestamp.
     */
    const_iterator trackTimestamps(const Timestamp& first, const Timestamp& last);

    /**
     * Stop tracking the timestamp that pos points to.
     */
    void untrackTimestamps(OplogVisibilityManager::const_iterator pos);

    /**
     * Returns the current visibility timestamp.
     */
    Timestamp getOplogVisibilityTimestamp() const;

    /**
     * Manually set the visibility timestamp to the timestamp passed in.
     * It's not allowed to advance the visibility timestamp if there are other timestamps being
     * tracked.
     */
    void setOplogVisibilityTimestamp(const Timestamp& visibilityTimestamp);

    /**
     * Waits for a timestamp to become visible (that is, no holes exist up to this timestamp).
     */
    void waitForTimestampToBeVisible(OperationContext* opCtx, const Timestamp& ts);

    /**
     * Waits for all committed writes at this time to become visible (that is, until no holes exist
     * in the oplog up to the time we start waiting).
     */
    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx, bool primaryOnly);

private:
    // Updates the oplog visibility timestamp and returns true if a visibilty update occurred.
    bool _setOplogVisibilityTimestamp(WithLock lock, const Timestamp& visibilityTimestamp);
    // Protects timestamp related variables and ensures operations are thread safe.
    stdx::mutex _mutex;
    // Latest oplog timestamp that has been handed out.
    Timestamp _latestTimeSeen;
    // Timestamp indicating point in the oplog with no holes.
    // Can be read without holding the _mutex due to its atomic nature.
    AtomicWord<Timestamp> _oplogVisibilityTimestamp;
    // List of timestamps tracked.
    SlottedTimestampList _oplogTimestampList;
    // The recordStore of the oplog collection.
    RecordStore* _rs = nullptr;
    // Signaled when oplog visibility has been updated.
    mutable stdx::condition_variable _oplogEntriesBecameVisibleCV;
};

}  // namespace repl
}  // namespace mongo
