// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/initial_sync/repl_sync_shared_data.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {
class InitialSyncSharedData final : public ReplSyncSharedData {
private:
    class RetryingOperation;

public:
    typedef boost::optional<RetryingOperation> RetryableOperation;

    InitialSyncSharedData(int rollBackId, Milliseconds allowedOutageDuration, ClockSource* clock)
        : ReplSyncSharedData(clock),
          _rollBackId(rollBackId),
          _allowedOutageDuration(allowedOutageDuration) {}

    int getRollBackId() const {
        return _rollBackId;
    }

    int getRetryingOperationsCount(WithLock lk) {
        return _retryingOperationsCount;
    }

    int getTotalRetries(WithLock lk) {
        return _totalRetries;
    }

    /**
     * Sets the initial sync ID of the sync source.
     */
    void setInitialSyncSourceId(WithLock, boost::optional<UUID> syncSourceId) {
        _initialSyncSourceId = syncSourceId;
    }

    /**
     * Gets the previously-set initial sync ID of the sync source.
     */
    boost::optional<UUID> getInitialSyncSourceId(WithLock) {
        return _initialSyncSourceId;
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
     * Returns the total time the sync source may be unreachable in a single outage before
     * shouldRetryOperation() returns false.
     */
    Milliseconds getAllowedOutageDuration(WithLock lk) {
        return _allowedOutageDuration;
    }

    [[MONGO_MOD_PRIVATE]] void setAllowedOutageDuration_forTest(
        WithLock, Milliseconds allowedOutageDuration) {
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
                std::lock_guard<InitialSyncSharedData> lk(*_sharedData);
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

        InitialSyncSharedData* getSharedData() {
            return _sharedData;
        }

    private:
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

    // Rollback ID at start of initial sync.
    const int _rollBackId;

    /**
     * This object must be locked when accessing the members below.
     */

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

    // Time allowed for an outage before "shouldRetryOperation" returns false.
    Milliseconds _allowedOutageDuration;

    // Operation that may currently be retrying.
    RetryableOperation _retryableOp;

    // The initial sync ID on the source at the start of data cloning.
    boost::optional<UUID> _initialSyncSourceId;
};
}  // namespace repl
}  // namespace mongo
