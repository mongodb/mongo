// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/slotted_timestamp_list.h"
#include "mongo/util/modules.h"

#include <mutex>

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
    std::mutex _mutex;
    // Latest oplog timestamp that has been handed out.
    Timestamp _latestTimeSeen;
    // Timestamp indicating point in the oplog with no holes.
    // Can be read without holding the _mutex due to its atomic nature.
    Atomic<Timestamp> _oplogVisibilityTimestamp;
    // List of timestamps tracked.
    SlottedTimestampList _oplogTimestampList;
    // The recordStore of the oplog collection.
    RecordStore* _rs = nullptr;
    // Signaled when oplog visibility has been updated.
    mutable stdx::condition_variable _oplogEntriesBecameVisibleCV;
};

}  // namespace repl
}  // namespace mongo
