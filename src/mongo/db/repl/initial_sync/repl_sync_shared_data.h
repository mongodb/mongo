// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/modules.h"

#include <mutex>

namespace mongo {
namespace repl {
class ReplSyncSharedData {
public:
    ReplSyncSharedData(ClockSource* clock) : _clock(clock) {}
    virtual ~ReplSyncSharedData() {}

    ClockSource* getClock() const {
        return _clock;
    }

    /**
     * BasicLockable C++ methods; they merely delegate to the mutex.
     * The presence of these methods means we can use std::unique_lock<ReplSyncSharedData> and
     * std::lock_guard<ReplSyncSharedData>.
     */
    void lock();

    void unlock();

    /**
     * In all cases below, the lock must be a lock on this object itself for access to be valid.
     */

    Status getStatus(WithLock lk);

    /**
     * Sets the status to the new status if and only if the old status is "OK".
     */
    void setStatusIfOK(WithLock lk, Status newStatus);

private:
    // Clock source used for timing outages and recording stats.
    ClockSource* const _clock;

    mutable std::mutex _mutex;

    // Status of the entire sync process.  All syncing tasks should exit if this becomes non-OK.
    Status _status = Status::OK();
};
}  // namespace repl
}  // namespace mongo
