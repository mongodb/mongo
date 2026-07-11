// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace mongo {

/**
 * Manages oplog visibility.
 *
 * On demand, queries the storage engine's allDurable timestamp value and updates the oplog read
 * timestamp. This is done synchronously when calls to triggerOplogVisibilityUpdate() provide a
 * timestamp greater than the current visibility timestamp.
 *
 * The allDurable timestamp is the in-memory timestamp behind which there are no oplog holes
 * in-memory. Note that allDurable is the timestamp that has no holes in-memory, which may NOT be
 * the case on disk, despite 'durable' in the name.
 *
 * The oplog read timestamp is used to read from the oplog with forward cursors, in order to ensure
 * readers never see 'holes' in the oplog and thereby miss data that was not yet committed when
 * scanning passed. Out-of-order primary writes allow writes with later timestamps to be committed
 * before writes assigned earlier timestamps, creating oplog 'holes'.
 *
 * repl::OplogVisibilityManager does something very similar to this class. It is intended to be a
 * replacement for this class, but has not yet been enabled.
 *
 * TODO(SERVER-85788): If it's enabled without removing this class, update the comment to reflect
 * the new relationship.
 */
class [[MONGO_MOD_PUBLIC]] StorageOplogManager {
    StorageOplogManager(const StorageOplogManager&) = delete;
    StorageOplogManager& operator=(const StorageOplogManager&) = delete;

public:
    StorageOplogManager() = default;
    ~StorageOplogManager() = default;

    /**
     * Starts the oplog manager, initializing the oplog read timestamp with the highest oplog
     * timestamp. Every call to start() must be followed by at least one call to stop() before
     * start() can be called again.
     */
    void start(OperationContext*, const KVEngine&, RecordStore& oplog);

    /**
     * Stops the oplog manager if it is running for the given oplog.
     *
     * If `oplog` is non-null, the manager will only stop if `oplog` is the same record store as was
     * passed to the most recent call to `start()`. If it is null, the manager will stop
     * unconditionally.
     */
    void stop(const RecordStore* oplog);

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
    std::string_view getIdent() const;

    bool isRunning_forTest() const {
        std::lock_guard lk(_oplogVisibilityStateMutex);
        return _oplog != nullptr;
    }

    bool isRunningForSpecificRS_forTest(const RecordStore* oplog) const {
        std::lock_guard lk(_oplogVisibilityStateMutex);
        return _oplog == oplog;
    }

private:
    enum class VisibilityUpdateResult {
        NotUpdated,
        Updated,
        Stopped,
    };
    VisibilityUpdateResult _updateVisibility(std::unique_lock<std::mutex>&,
                                             const KVEngine&,
                                             const RecordStore::Capped& oplog);

    void _setOplogReadTimestamp(WithLock, uint64_t newTimestamp);

    std::string _oplogIdent;

    Atomic<unsigned long long> _oplogReadTimestamp{0};

    stdx::thread _oplogVisibilityThread;

    // Signaled to trigger the oplog visibility thread to run.
    mutable stdx::condition_variable _oplogVisibilityThreadCV;

    // Signaled when oplog visibility has been updated.
    mutable stdx::condition_variable _oplogEntriesBecameVisibleCV;

    // Protects the state below.
    mutable std::mutex _oplogVisibilityStateMutex;

    // The record store which the thread is currently running for. May be null while the thread is
    // running if the thread is in the process of shutting down.
    RecordStore* _oplog = nullptr;

    // Whether an oplog to oplog visibility is being triggered.
    bool _triggerOplogVisibilityUpdate = false;

    // The number of operations waiting for more of the oplog to become visible, to avoid update
    // delays for batching.
    int64_t _opsWaitingForOplogVisibilityUpdate = 0;
};
}  // namespace mongo
