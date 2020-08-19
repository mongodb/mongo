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

#include <mutex>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/wire_version.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace repl {
class ReplSyncSharedData {
protected:
    class RetryingOperation;

public:
    typedef boost::optional<RetryingOperation> RetryableOperation;

    ReplSyncSharedData(int rollBackId, Milliseconds allowedOutageDuration, ClockSource* clock)
        : _rollBackId(rollBackId), _clock(clock), _allowedOutageDuration(allowedOutageDuration) {}
    virtual ~ReplSyncSharedData() {}

    // TODO(SERVER-50492): Move this into InitialSyncSharedData once the base cloner has been split.
    int getRollBackId() const {
        return _rollBackId;
    }

    ClockSource* getClock() const {
        return _clock;
    }

    /**
     * BasicLockable C++ methods; they merely delegate to the mutex.
     * The presence of these methods means we can use stdx::unique_lock<ReplSyncSharedData> and
     * stdx::lock_guard<ReplSyncSharedData>.
     */
    void lock();

    void unlock();

    /**
     * In all cases below, the lock must be a lock on this object itself for access to be valid.
     */

    Status getStatus(WithLock lk);

    void setStatus(WithLock lk, Status newStatus);

    /**
     * Sets the status to the new status if and only if the old status is "OK".
     */
    void setStatusIfOK(WithLock lk, Status newStatus);

    /**
     * Sets the wire version of the sync source.
     * TODO(SERVER-50492): Move this into InitialSyncSharedData once the base cloner has been split.
     */
    virtual void setSyncSourceWireVersion(WithLock, WireVersion wireVersion) = 0;

    /**
     * Returns the wire version of the sync source, if previously set.
     * TODO(SERVER-50492): Move this into InitialSyncSharedData once the base cloner has been split.
     */
    virtual boost::optional<WireVersion> getSyncSourceWireVersion(WithLock) = 0;

    /**
     * Sets the initial sync ID of the sync source.
     * TODO(SERVER-50492): Move this into InitialSyncSharedData once the base cloner has been split.
     */
    virtual void setInitialSyncSourceId(WithLock, boost::optional<UUID> syncSourceId) = 0;

    /**
     * Gets the previously-set initial sync ID of the sync source.
     * TODO(SERVER-50492): Move this into InitialSyncSharedData once the base cloner has been split.
     */
    virtual boost::optional<UUID> getInitialSyncSourceId(WithLock) = 0;

    /**
     * Returns true if the operation should be retried, false if it has timed out.
     * TODO(SERVER-50492): Move this into InitialSyncSharedData once the base cloner has been split.
     */
    virtual bool shouldRetryOperation(WithLock lk, RetryableOperation* retryableOp) = 0;

    /**
     * Decrements the number of retrying operations.  If now zero, clear syncSourceUnreachableSince
     * and update _totalTimeUnreachable.
     * Returns the new number of retrying operations.
     * TODO(SERVER-50492): Move this into InitialSyncSharedData once the base cloner has been split.
     */
    virtual int decrementRetryingOperations(WithLock lk) = 0;

    /**
     * Returns the total time the sync source may be unreachable in a single outage before
     * shouldRetryOperation() returns false.
     * TODO(SERVER-50492): Move this into InitialSyncSharedData once the base cloner has been split.
     */
    Milliseconds getAllowedOutageDuration(WithLock lk) {
        return _allowedOutageDuration;
    }

    // TODO(SERVER-50492): Move this into InitialSyncSharedData once the base cloner has been split.
    void setAllowedOutageDuration_forTest(WithLock, Milliseconds allowedOutageDuration) {
        _allowedOutageDuration = allowedOutageDuration;
    }

protected:
    // TODO(SERVER-50492): Move this into InitialSyncSharedData once the base cloner has been split.
    class RetryingOperation {
    public:
        RetryingOperation(ReplSyncSharedData* sharedData) : _sharedData(sharedData) {}
        // This class is a non-copyable RAII class.
        RetryingOperation(const RetryingOperation&) = delete;
        RetryingOperation(RetryingOperation&&) = delete;
        ~RetryingOperation() {
            if (_sharedData) {
                stdx::lock_guard<ReplSyncSharedData> lk(*_sharedData);
                release(lk);
            }
        }

        RetryingOperation& operator=(const RetryingOperation&) = delete;
        RetryingOperation& operator=(RetryingOperation&&) = default;

        /**
         * release() is used by shouldRetryOperation to allow destroying a RetryingOperation
         * while holding the lock.
         */
        void release(WithLock lk) {
            _sharedData->decrementRetryingOperations(lk);
            _sharedData = nullptr;
        }

        ReplSyncSharedData* getSharedData() {
            return _sharedData;
        }

    private:
        ReplSyncSharedData* _sharedData;
    };

private:
    // Rollback ID at start of initial sync.
    // TODO(SERVER-50492): Move this into InitialSyncSharedData once the base cloner has been split.
    const int _rollBackId;

    // Clock source used for timing outages and recording stats.
    ClockSource* const _clock;

    mutable Mutex _mutex = MONGO_MAKE_LATCH("ReplSyncSharedData::_mutex"_sd);

    // Time allowed for an outage before "shouldRetryOperation" returns false.
    // TODO(SERVER-50492): Move this into InitialSyncSharedData once the base cloner has been split.
    Milliseconds _allowedOutageDuration;

    // Status of the entire sync process.  All syncing tasks should exit if this becomes non-OK.
    Status _status = Status::OK();
};
}  // namespace repl
}  // namespace mongo