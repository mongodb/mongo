// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replication_state_transition_lock_guard.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/shard_role/transaction_resources.h"
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
    // TODO SERVER-106669: Remove this class when we delete the feature flag.
    if (gFeatureFlagIntentRegistration.isEnabled()) {
        _result = LOCK_OK;
        return;
    }
    // Enqueue a lock request for the RSTL.
    _result = shard_role_details::getLocker(_opCtx)->lockRSTLBegin(_opCtx, _mode);
}

void ReplicationStateTransitionLockGuard::_unlock() {
    if (gFeatureFlagIntentRegistration.isEnabled()) {
        _result = LOCK_INVALID;
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
