/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"

#include <cstdint>

namespace mongo {

/**
 * Manages oplog visibility.
 *
 * On demand, queries WiredTiger's all_durable timestamp value and updates the oplog read timestamp.
 * This is done synchronously when calls to triggerOplogVisibilityUpdate() provide a timestamp
 * greater than the current visibility timestamp.
 *
 * The WT all_durable timestamp is the in-memory timestamp behind which there are no oplog holes
 * in-memory. Note, all_durable is the timestamp that has no holes in-memory, which may NOT be
 * the case on disk, despite 'durable' in the name.
 *
 * The oplog read timestamp is used to read from the oplog with forward cursors, in order to ensure
 * readers never see 'holes' in the oplog and thereby miss data that was not yet committed when
 * scanning passed. Out-of-order primary writes allow writes with later timestamps to be committed
 * before writes assigned earlier timestamps, creating oplog 'holes'.
 */
class WiredTigerOplogManager {
    WiredTigerOplogManager(const WiredTigerOplogManager&) = delete;
    WiredTigerOplogManager& operator=(const WiredTigerOplogManager&) = delete;

public:
    WiredTigerOplogManager() = default;
    ~WiredTigerOplogManager() = default;

    /**
     * Starts the oplog manager, initializing the oplog read timestamp with the highest oplog
     * timestamp.
     */
    void start(OperationContext*, const KVEngine&, RecordStore& oplog, bool isReplSet);

    /**
     * Stops the oplog manager.
     */
    void stop();

    /**
     * Updates the oplog read timestamp if the visibility timestamp is behind the provided
     * `commitTimestamp`, and notifies any capped waiters on the oplog if the visibility point is
     * advanced.
     *
     * Callers must ensure this call is not concurrent with a call to `initialize`, as this is not
     * thread-safe.
     */
    void triggerOplogVisibilityUpdate();

    /**
     * Waits for all committed writes at this time to become visible (that is, until no holes exist
     * in the oplog up to the time we start waiting.)
     */
    void waitForAllEarlierOplogWritesToBeVisible(const RecordStore* oplogRecordStore,
                                                 OperationContext* opCtx);

    /**
     * The oplogReadTimestamp is the read timestamp used for forward cursor oplog reads to prevent
     * such readers from missing any entries in the oplog that may not yet have committed ('holes')
     * when the scan passes along the data. The 'oplogReadTimestamp' is a guaranteed 'no holes'
     * point in the oplog.
     *
     * Holes in the oplog occur due to out-of-order primary writes, where a write with a later
     * assigned timestamp can commit before a write assigned an earlier timestamp.
     *
     * This is a thread-safe call at any point.
     */
    std::uint64_t getOplogReadTimestamp() const;
    void setOplogReadTimestamp(Timestamp ts);

    /**
     * Returns an empty string if `initialize` hasn't been called.
     */
    StringData getIdent() const;

private:
    enum class VisibilityUpdateResult {
        NotUpdated,
        Updated,
        Stopped,
    };
    VisibilityUpdateResult _updateVisibility(stdx::unique_lock<stdx::mutex>&,
                                             const KVEngine&,
                                             const RecordStore::Capped& oplog);

    void _setOplogReadTimestamp(WithLock, uint64_t newTimestamp);

    std::string _oplogIdent;

    AtomicWord<unsigned long long> _oplogReadTimestamp{0};

    stdx::thread _oplogVisibilityThread;

    // Signaled to trigger the oplog visibility thread to run.
    mutable stdx::condition_variable _oplogVisibilityThreadCV;

    // Signaled when oplog visibility has been updated.
    mutable stdx::condition_variable _oplogEntriesBecameVisibleCV;

    // Protects the state below.
    mutable stdx::mutex _oplogVisibilityStateMutex;

    // Whether this oplog manager is currently running.
    bool _running = false;

    // Whether an oplog to oplog visibility is being triggered.
    bool _triggerOplogVisibilityUpdate = false;

    // The number of operations waiting for more of the oplog to become visible, to avoid update
    // delays for batching.
    int64_t _opsWaitingForOplogVisibilityUpdate = 0;
};
}  // namespace mongo
