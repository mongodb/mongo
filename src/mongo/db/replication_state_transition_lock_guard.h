/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <boost/optional.hpp>

namespace mongo {

class OperationContext;

namespace repl {

/**
 * This object handles acquiring the RSTL for replication state transitions, as well as any actions
 * that need to happen in between enqueuing the RSTL request and waiting for it to be granted.
 */
class MONGO_MOD_PUB ReplicationStateTransitionLockGuard {
    ReplicationStateTransitionLockGuard(const ReplicationStateTransitionLockGuard&) = delete;
    ReplicationStateTransitionLockGuard& operator=(const ReplicationStateTransitionLockGuard&) =
        delete;

public:
    class EnqueueOnly {};

    /**
     * Acquires the RSTL in the requested mode (typically mode X).
     */
    ReplicationStateTransitionLockGuard(OperationContext* opCtx, LockMode mode);

    /**
     * Enqueues RSTL but does not block on lock acquisition.
     * Must call waitForLockUntil() to complete locking process.
     */
    ReplicationStateTransitionLockGuard(OperationContext* opCtx, LockMode mode, EnqueueOnly);

    ReplicationStateTransitionLockGuard(ReplicationStateTransitionLockGuard&&);

    ~ReplicationStateTransitionLockGuard();

    /**
     * Waits for RSTL to be granted.
     */
    void waitForLockUntil(Date_t deadline, const Locker::LockTimeoutCallback& onTimeout = nullptr);

    /**
     * Release and reacquire the RSTL.
     */
    void release();
    void reacquire();

    bool isLocked() const {
        return _result == LOCK_OK;
    }

private:
    void _enqueueLock();
    void _unlock();

    OperationContext* const _opCtx;
    LockMode _mode;
    LockResult _result;
};

}  // namespace repl
}  // namespace mongo
