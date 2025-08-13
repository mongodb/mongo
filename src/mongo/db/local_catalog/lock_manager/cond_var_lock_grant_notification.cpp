/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/lock_manager/cond_var_lock_grant_notification.h"

#include "mongo/db/operation_context.h"

namespace mongo {

LockResult CondVarLockGrantNotification::wait(Milliseconds timeout) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    return _cond.wait_for(
               lock, timeout.toSystemDuration(), [this] { return _result != LOCK_INVALID; })
        ? _result
        : LOCK_TIMEOUT;
}

LockResult CondVarLockGrantNotification::wait(OperationContext* opCtx, Milliseconds timeout) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
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
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    invariant(_result == LOCK_INVALID);
    _result = result;

    _cond.notify_all();
}

}  // namespace mongo
