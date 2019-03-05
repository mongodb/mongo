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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/operation_context.h"

namespace mongo {
namespace repl {

ReplicationStateTransitionLockGuard::ReplicationStateTransitionLockGuard(OperationContext* opCtx,
                                                                         LockMode mode)
    : ReplicationStateTransitionLockGuard(opCtx, mode, EnqueueOnly()) {
    waitForLockUntil(Date_t::max());
}

ReplicationStateTransitionLockGuard::ReplicationStateTransitionLockGuard(OperationContext* opCtx,
                                                                         LockMode mode,
                                                                         EnqueueOnly)
    : _opCtx(opCtx), _mode(mode) {
    _enqueueLock();
}

ReplicationStateTransitionLockGuard::ReplicationStateTransitionLockGuard(
    ReplicationStateTransitionLockGuard&& other)
    : _opCtx(other._opCtx), _result(other._result) {
    other._result = LOCK_INVALID;
}

ReplicationStateTransitionLockGuard::~ReplicationStateTransitionLockGuard() {
    _unlock();
}

void ReplicationStateTransitionLockGuard::waitForLockUntil(mongo::Date_t deadline) {
    // We can return early if the lock request was already satisfied.
    if (_result == LOCK_OK) {
        return;
    }

    _result = LOCK_INVALID;
    // Wait for the completion of the lock request for the RSTL.
    _opCtx->lockState()->lockRSTLComplete(_opCtx, _mode, deadline);
    _result = LOCK_OK;
}

void ReplicationStateTransitionLockGuard::release() {
    _unlock();
}

void ReplicationStateTransitionLockGuard::reacquire() {
    _enqueueLock();
    waitForLockUntil(Date_t::max());
}

void ReplicationStateTransitionLockGuard::_enqueueLock() {
    // Enqueue a lock request for the RSTL.
    _result = _opCtx->lockState()->lockRSTLBegin(_opCtx, _mode);
}

void ReplicationStateTransitionLockGuard::_unlock() {
    // waitForLockUntil() must be called after enqueue. It either times out or succeeds,
    // so we should never see LOCK_WAITING here. This also means between the enqueue and
    // waitForLockUntil(), no exception is accepted. We could call lockRSTLComplete() with
    // timeout 0 here for pending locks to clean up the lock's state, but it's clearer to enforce
    // the exception-free pattern.
    invariant(_result != LOCK_WAITING);
    if (isLocked()) {
        _opCtx->lockState()->unlock(resourceIdReplicationStateTransitionLock);
    }
    _result = LOCK_INVALID;
}

}  // namespace repl
}  // namespace mongo
