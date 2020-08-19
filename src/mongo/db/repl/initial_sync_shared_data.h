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

#include "mongo/db/repl/repl_sync_shared_data.h"
#include "mongo/db/server_options.h"

namespace mongo {
namespace repl {
class InitialSyncSharedData final : public ReplSyncSharedData {
private:
    class RetryingOperation;

public:
    InitialSyncSharedData(int rollBackId, Milliseconds allowedOutageDuration, ClockSource* clock)
        : ReplSyncSharedData(rollBackId, allowedOutageDuration, clock) {}

    int getRetryingOperationsCount(WithLock lk) {
        return _retryingOperationsCount;
    }

    int getTotalRetries(WithLock lk) {
        return _totalRetries;
    }

    /**
     * Sets the wire version of the sync source.
     */
    void setSyncSourceWireVersion(WithLock, WireVersion wireVersion) final {
        _syncSourceWireVersion = wireVersion;
    }

    /**
     * Returns the wire version of the sync source, if previously set.
     */
    boost::optional<WireVersion> getSyncSourceWireVersion(WithLock) final {
        return _syncSourceWireVersion;
    }

    /**
     * Sets the initial sync ID of the sync source.
     */
    void setInitialSyncSourceId(WithLock, boost::optional<UUID> syncSourceId) final {
        _initialSyncSourceId = syncSourceId;
    }

    /**
     * Gets the previously-set initial sync ID of the sync source.
     */
    boost::optional<UUID> getInitialSyncSourceId(WithLock) final {
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
    bool shouldRetryOperation(WithLock lk, RetryableOperation* retryableOp) override;

private:
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
    int decrementRetryingOperations(WithLock lk) override;

    void incrementTotalRetries(WithLock lk) {
        _totalRetries++;
    }

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

    // Operation that may currently be retrying.
    RetryableOperation _retryableOp;

    // The sync source wire version at the start of data cloning.
    boost::optional<WireVersion> _syncSourceWireVersion;

    // The initial sync ID on the source at the start of data cloning.
    boost::optional<UUID> _initialSyncSourceId;
};
}  // namespace repl
}  // namespace mongo
