/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/db/server_options.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/clock_source.h"

namespace mongo {
namespace repl {
class InitialSyncSharedData {
private:
    class RetryingOperation;

public:
    typedef boost::optional<RetryingOperation> RetryableOperation;

    InitialSyncSharedData(int rollBackId, Milliseconds allowedOutageDuration, ClockSource* clock)
        : _rollBackId(rollBackId), _clock(clock), _allowedOutageDuration(allowedOutageDuration) {}

    int getRollBackId() const {
        return _rollBackId;
    }

    ClockSource* getClock() const {
        return _clock;
    }

    /**
     * In all cases below, the lock must be a lock on this object itself for access to be valid.
     */

    Status getInitialSyncStatus(WithLock lk) {
        return _initialSyncStatus;
    }

    void setInitialSyncStatus(WithLock lk, Status newStatus) {
        _initialSyncStatus = newStatus;
    }

    /**
     * Sets the initialSyncStatus to the new status if and only if the old status is "OK".
     */
    void setInitialSyncStatusIfOK(WithLock lk, Status newStatus) {
        if (_initialSyncStatus.isOK())
            _initialSyncStatus = newStatus;
    }

    int getRetryingOperationsCount(WithLock lk) {
        return _retryingOperationsCount;
    }

    int getTotalRetries(WithLock lk) {
        return _totalRetries;
    }

    /**
     * Returns the total time the sync source has been unreachable, including any current outage.
     */
    Milliseconds getTotalTimeUnreachable(WithLock lk);

    /**
     * Returns the total time the sync source has been unreachable in the current outage.
     * Returns Milliseconds::min() if there is no current outage.
     */
    Milliseconds getCurrentOutageDuration(WithLock lk);

    /**
     * Returns the total time the sync source may be unreachable in a single outage before
     * shouldRetryOperation() returns false.
     */
    Milliseconds getAllowedOutageDuration(WithLock lk) {
        return _allowedOutageDuration;
    }

    /**
     * Returns the time the current outage (if any) began.  Returns Date_t() if no outage in
     * progress.
     */
    Date_t getSyncSourceUnreachableSince(WithLock lk) {
        return _syncSourceUnreachableSince;
    }

    /**
     * shouldRetryOperation() is the interface for retries.  For each retryable operation, declare a
     * RetryableOperation which is passed to this method.  When the operation succeeds, destroy the
     * RetryableOperation (outside the lock) or assign boost::none to it.
     *
     * Returns true if the operation should be retried, false if it has timed out.
     */
    bool shouldRetryOperation(WithLock lk, RetryableOperation* retryableOp);

    /**
     * BasicLockable C++ methods; they merely delegate to the mutex.
     * The presence of these methods means we can use stdx::unique_lock<InitialSyncSharedData> and
     * stdx::lock_guard<InitialSyncSharedData>.
     */
    void lock() {
        _mutex.lock();
    }

    void unlock() {
        _mutex.unlock();
    }

    void setAllowedOutageDuration_forTest(WithLock, Milliseconds allowedOutageDuration) {
        _allowedOutageDuration = allowedOutageDuration;
    }

private:
    class RetryingOperation {
    public:
        RetryingOperation(InitialSyncSharedData* sharedData) : _sharedData(sharedData) {}
        // This class is a non-copyable RAII class.
        RetryingOperation(const RetryingOperation&) = delete;
        RetryingOperation(RetryingOperation&&) = delete;
        ~RetryingOperation() {
            if (_sharedData) {
                stdx::lock_guard<InitialSyncSharedData> lk(*_sharedData);
                release(lk);
            }
        }

        RetryingOperation& operator=(const RetryingOperation&) = delete;
        RetryingOperation& operator=(RetryingOperation&&) = default;

    private:
        friend class InitialSyncSharedData;

        /**
         * release() is used by shouldRetryOperation to allow destroying a RetryingOperation
         * while holding the lock.
         */
        void release(WithLock lk) {
            _sharedData->decrementRetryingOperations(lk);
            _sharedData = nullptr;
        }

        InitialSyncSharedData* _sharedData;
    };

    /**
     * Increment the number of retrying operations, set syncSourceUnreachableSince if this is the
     * only retrying operation. This is used when an operation starts retrying.
     *
     * Returns the new number of retrying operations.
     */
    int incrementRetryingOperations(WithLock lk);

    /**
     * Decrements the number of retrying operations.  If now zero, clear syncSourceUnreachableSince
     * and update _totalTimeUnreachable.
     * Returns the new number of retrying operations.
     */
    int decrementRetryingOperations(WithLock lk);

    void incrementTotalRetries(WithLock lk) {
        _totalRetries++;
    }

    // The const members above the mutex may be accessed without the mutex.

    // Rollback ID at start of initial sync
    const int _rollBackId;

    // Clock source used for timing outages
    ClockSource* const _clock;

    // This mutex controls access to all members below it.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("InitialSyncSharedData::_mutex"_sd);

    // Time allowed for an outage before "shouldRetryOperation" returns false.
    Milliseconds _allowedOutageDuration;

    // Status of the entire initial sync.  All initial sync tasks should exit if this becomes
    // non-OK.
    Status _initialSyncStatus = Status::OK();

    // Number of operations currently being retried due to a transient error.
    int _retryingOperationsCount = 0;

    // Number of total retry attempts for all operations.  Does not include initial attempts,
    // so should normally be 0.
    int _totalRetries = 0;

    // If any operation is currently retrying, the earliest time at which any operation detected
    // a transient network error.  Otherwise is Date_t().
    Date_t _syncSourceUnreachableSince;

    // The total time across all outages in this initial sync attempt, but excluding any current
    // outage, that we were retrying because we were unable to reach the sync source.
    Milliseconds _totalTimeUnreachable;
};
}  // namespace repl
}  // namespace mongo
