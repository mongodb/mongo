// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
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
class [[MONGO_MOD_PUBLIC]] ReplicationStateTransitionLockGuard {
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
