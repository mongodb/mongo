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

#include "mongo/db/replication_state_transition_lock_guard.h"

#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


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
    : _opCtx(other._opCtx), _mode(other._mode), _result(other._result) {
    other._result = LockResult::LOCK_INVALID;
}

ReplicationStateTransitionLockGuard::~ReplicationStateTransitionLockGuard() {
    _unlock();
}

void ReplicationStateTransitionLockGuard::waitForLockUntil(
    mongo::Date_t deadline, const Locker::LockTimeoutCallback& onTimeout) {
    if (gFeatureFlagIntentRegistration.isEnabled()) {
        return;
    }
    // We can return early if the lock request was already satisfied.
    if (_result == LOCK_OK) {
        return;
    }

    _result = LOCK_INVALID;
    // Wait for the completion of the lock request for the RSTL.
    shard_role_details::getLocker(_opCtx)->lockRSTLComplete(_opCtx, _mode, deadline, onTimeout);
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
    if (gFeatureFlagIntentRegistration.isEnabled()) {
        return;
    }
    // Enqueue a lock request for the RSTL.
    _result = shard_role_details::getLocker(_opCtx)->lockRSTLBegin(_opCtx, _mode);
}

void ReplicationStateTransitionLockGuard::_unlock() {
    if (gFeatureFlagIntentRegistration.isEnabled()) {
        return;
    }
    if (_result == LockResult::LOCK_INVALID) {
        return;
    }

    // If ReplicationStateTransitionLockGuard is called in a WriteUnitOfWork, we won't accept
    // any exceptions to be thrown between _enqueueLock and waitForLockUntil because that would
    // delay cleaning up any failed RSTL lock attempt state from lock manager.
    invariant(
        !(_result == LOCK_WAITING && shard_role_details::getLocker(_opCtx)->inAWriteUnitOfWork()),
        str::stream() << "Lock result: " << _result << ". In a write unit of work: "
                      << shard_role_details::getLocker(_opCtx)->inAWriteUnitOfWork());
    shard_role_details::getLocker(_opCtx)->unlock(resourceIdReplicationStateTransitionLock);
    _result = LOCK_INVALID;
}

}  // namespace repl
}  // namespace mongo
