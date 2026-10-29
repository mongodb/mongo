// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/lock_manager/cond_var_lock_grant_notification.h"

#include "mongo/db/operation_context.h"

namespace mongo {

LockResult CondVarLockGrantNotification::wait(Milliseconds timeout) {
    std::unique_lock<std::mutex> lock(_mutex);
    return _cond.wait_for(
               lock, timeout.toSystemDuration(), [this] { return _result != LOCK_INVALID; })
        ? _result
        : LOCK_TIMEOUT;
}

LockResult CondVarLockGrantNotification::wait(OperationContext* opCtx, Milliseconds timeout) {
    std::unique_lock<std::mutex> lock(_mutex);
    if (opCtx->waitForConditionOrInterruptFor(
            _cond, lock, timeout, [this] { return _result != LOCK_INVALID; })) {
        // Because waitForConditionOrInterruptFor evaluates the predicate before checking for
        // interrupt, it is possible that a killed operation can acquire a lock if the request is
        // granted quickly. For that reason, it is necessary to check if the operation has been
        // killed at least once before accepting the lock grant.
        opCtx->checkForInterrupt();
        return _result;
    }
    return LOCK_TIMEOUT;
}

void CondVarLockGrantNotification::notify(ResourceId resId, LockResult result) {
    std::unique_lock<std::mutex> lock(_mutex);
    invariant(_result == LOCK_INVALID);
    _result = result;

    _cond.notify_all();
}

}  // namespace mongo
