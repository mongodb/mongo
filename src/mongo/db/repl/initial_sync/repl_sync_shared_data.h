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

#include "mongo/base/status.h"
#include "mongo/stdx/mutex.h"
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
     * The presence of these methods means we can use stdx::unique_lock<ReplSyncSharedData> and
     * stdx::lock_guard<ReplSyncSharedData>.
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

    mutable stdx::mutex _mutex;

    // Status of the entire sync process.  All syncing tasks should exit if this becomes non-OK.
    Status _status = Status::OK();
};
}  // namespace repl
}  // namespace mongo
