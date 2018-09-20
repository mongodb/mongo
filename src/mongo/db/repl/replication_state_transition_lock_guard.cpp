/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/session_catalog.h"
#include "mongo/db/session_killer.h"
#include "mongo/db/transaction_participant.h"

namespace mongo {
namespace repl {

ReplicationStateTransitionLockGuard::ReplicationStateTransitionLockGuard(OperationContext* opCtx,
                                                                         const Args& args)
    : _opCtx(opCtx), _args(args) {

    // First enqueue the request for the global X lock.
    _globalLock.emplace(opCtx,
                        MODE_X,
                        args.lockDeadline,
                        Lock::InterruptBehavior::kThrow,
                        Lock::GlobalLock::EnqueueOnly());

    // Next prevent any Sessions from being created or checked out.
    _preventCheckingOutSessions.emplace(SessionCatalog::get(opCtx));

    // If we're going to be killing all user operations do it before waiting for the global lock
    // and for all Sessions to be checked in as killing all running user ops may make those things
    // happen faster.
    if (args.killUserOperations) {
        ServiceContext* environment = opCtx->getServiceContext();
        environment->killAllUserOperations(opCtx, ErrorCodes::InterruptedDueToStepDown);
    }

    // Now wait for all Sessions to be checked in so we can iterate over all of them and abort
    // any in-progress transactions and yield and gather the LockSnapshots for all prepared
    // transactions.
    _preventCheckingOutSessions->waitForAllSessionsToBeCheckedIn(opCtx);
    killSessionsLocalAbortOrYieldAllTransactions(opCtx, &_yieldedLocks);

    // Now that all transactions have either aborted or yielded their locks, we can wait for the
    // global X lock to be taken successfully.
    _globalLock->waitForLockUntil(args.lockDeadline);
    uassert(ErrorCodes::ExceededTimeLimit,
            "Could not acquire the global lock before the deadline",
            _globalLock->isLocked());
}

ReplicationStateTransitionLockGuard::~ReplicationStateTransitionLockGuard() {
    invariant(_globalLock->isLocked());

    // Restore the locks for the prepared transactions, but put all requests for the global lock
    // into a TemporaryResourceQueue for the global resource.
    const ResourceId globalResId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL);
    LockManager::TemporaryResourceQueue tempGlobalResource(globalResId);
    for (auto&& pair : _yieldedLocks) {
        auto locker = pair.first;
        auto lockSnapshot = pair.second;

        locker->restoreLockStateWithTemporaryGlobalResource(
            _opCtx, lockSnapshot, &tempGlobalResource);
    }

    // Now atomically release the global X lock and restore the locks on the global resource from
    // the TemporaryResourceQueue that was populated with the Global lock requests from the yielded
    // locks from prepared transactions.
    _opCtx->lockState()->replaceGlobalLockStateWithTemporaryGlobalResource(&tempGlobalResource);
}

void ReplicationStateTransitionLockGuard::releaseGlobalLockForStepdownAttempt() {
    invariant(_globalLock->isLocked());
    _globalLock.reset();
}

void ReplicationStateTransitionLockGuard::reacquireGlobalLockForStepdownAttempt() {
    invariant(!_globalLock);

    UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
    _globalLock.emplace(_opCtx, MODE_X);
}

}  // namespace repl
}  // namespace mongo
