
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

#include "mongo/db/repl/replication_state_transition_lock_guard.h"

#include "mongo/db/kill_sessions_local.h"

namespace mongo {
namespace repl {

ReplicationStateTransitionLockGuard::ReplicationStateTransitionLockGuard(OperationContext* opCtx)
    : ReplicationStateTransitionLockGuard(opCtx, Args()) {}

ReplicationStateTransitionLockGuard::ReplicationStateTransitionLockGuard(OperationContext* opCtx,
                                                                         const Args& args)
    : _opCtx(opCtx), _args(args) {
    // Enqueue a lock request for the RSTL in mode X.
    LockResult result = _opCtx->lockState()->lockRSTLBegin(_opCtx);

    if (args.killUserOperations) {
        ServiceContext* environment = opCtx->getServiceContext();
        environment->killAllUserOperations(opCtx, ErrorCodes::InterruptedDueToStepDown);

        // Destroy all stashed transaction resources, in order to release locks.
        SessionKiller::Matcher matcherAllSessions(
            KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
        killSessionsLocalKillTransactions(
            opCtx, matcherAllSessions, ErrorCodes::InterruptedDueToStepDown);
    }

    // We can return early if the lock request was already satisfied.
    if (result == LOCK_OK) {
        return;
    }

    // Wait for the completion of the lock request for the RSTL in mode X.
    _opCtx->lockState()->lockRSTLComplete(opCtx, args.lockDeadline);
    uassert(ErrorCodes::ExceededTimeLimit,
            "Could not acquire the RSTL before the deadline",
            opCtx->lockState()->isRSTLExclusive());
}

ReplicationStateTransitionLockGuard::~ReplicationStateTransitionLockGuard() {
    invariant(_opCtx->lockState()->isRSTLExclusive());
    _opCtx->lockState()->unlock(resourceIdReplicationStateTransitionLock);
}

void ReplicationStateTransitionLockGuard::releaseRSTL() {
    invariant(_opCtx->lockState()->isRSTLExclusive());
    _opCtx->lockState()->unlock(resourceIdReplicationStateTransitionLock);
}

void ReplicationStateTransitionLockGuard::reacquireRSTL() {
    invariant(!_opCtx->lockState()->isRSTLLocked());

    UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
    _opCtx->lockState()->lock(_opCtx, resourceIdReplicationStateTransitionLock, MODE_X);
}

}  // namespace repl
}  // namespace mongo
