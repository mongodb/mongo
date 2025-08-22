/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/repl/auto_get_rstl_for_stepup_stepdown.h"

#include "mongo/db/curop.h"
#include "mongo/db/local_catalog/lock_manager/dump_lock_manager.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {

AutoGetRstlForStepUpStepDown::AutoGetRstlForStepUpStepDown(
    StepUpStepDownCoordinator* stepUpStepDownCoord,
    OperationContext* opCtx,
    const ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
    Date_t deadline)
    : _stepUpStepDownCoord(stepUpStepDownCoord), _opCtx(opCtx), _stateTransition(stateTransition) {
    invariant(_stepUpStepDownCoord);
    invariant(_opCtx);

    // The state transition should never be rollback within this class.
    invariant(_stateTransition != ReplicationCoordinator::OpsKillingStateTransitionEnum::kRollback);

    if (_stateTransition == ReplicationCoordinator::OpsKillingStateTransitionEnum::kStepDown)
        _stepUpStepDownCoord->autoGetRstlEnterStepDown();
    ScopeGuard callReplCoordExit([&] {
        if (_stateTransition == ReplicationCoordinator::OpsKillingStateTransitionEnum::kStepDown)
            _stepUpStepDownCoord->autoGetRstlExitStepDown();
    });
    int rstlTimeout = fassertOnLockTimeoutForStepUpDown.load();
    Date_t start{Date_t::now()};
    if (rstlTimeout > 0 && deadline - start > Seconds(rstlTimeout)) {
        deadline = start + Seconds(rstlTimeout);  // cap deadline
    }

    _rstlLock.emplace(_opCtx, MODE_X, ReplicationStateTransitionLockGuard::EnqueueOnly());

    ON_BLOCK_EXIT([&] { _stopAndWaitForKillOpThread(); });
    _startKillOpThread();

    // Wait for RSTL to be acquired.
    _rstlLock->waitForLockUntil(deadline, [opCtx, rstlTimeout, start] {
        if (rstlTimeout <= 0 || Date_t::now() - start < Seconds{rstlTimeout}) {
            return;
        }

        // Dump all locks to identify which thread(s) are holding RSTL.
        try {
            dumpLockManager();
        } catch (const DBException& e) {
            // If there are too many locks, dumpLockManager may fail.
            LOGV2_FATAL_CONTINUE(9222300, "Dumping locks failed", "error"_attr = e);
        }

        auto lockerInfo = shard_role_details::getLocker(opCtx)->getLockerInfo(
            CurOp::get(opCtx)->getLockStatsBase());
        BSONObjBuilder lockRep;
        lockerInfo.stats.report(&lockRep);

        LOGV2_FATAL_CONTINUE(
            5675600,
            "Time out exceeded waiting for RSTL, stepUp/stepDown is not possible thus "
            "calling abort() to allow cluster to progress",
            "lockRep"_attr = lockRep.obj());

#if defined(MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS)
        // Dump the stack of each thread.
        printAllThreadStacksBlocking();
#endif

        fassertFailed(7152000);
    });
    callReplCoordExit.dismiss();
};

AutoGetRstlForStepUpStepDown::~AutoGetRstlForStepUpStepDown() {
    if (_stateTransition == ReplicationCoordinator::OpsKillingStateTransitionEnum::kStepDown)
        _stepUpStepDownCoord->autoGetRstlExitStepDown();
}

void AutoGetRstlForStepUpStepDown::_startKillOpThread() {
    invariant(!_killOpThread);
    _killOpThread = std::make_unique<stdx::thread>([this] { _killOpThreadFn(); });
}

void AutoGetRstlForStepUpStepDown::_killOpThreadFn() {
    Client::initThread("RstlKillOpThread",
                       getGlobalServiceContext()->getService(ClusterRole::ShardServer),
                       Client::noSession(),
                       ClientOperationKillableByStepdown{false});

    LOGV2(21343, "Starting to kill user operations");
    auto uniqueOpCtx = cc().makeOperationContext();
    OperationContext* opCtx = uniqueOpCtx.get();

    // Set the reason for killing operations.
    ErrorCodes::Error killReason = ErrorCodes::InterruptedDueToReplStateChange;

    // This thread needs storage rollback to complete timely, so instruct the storage
    // engine to not do any extra eviction for this thread, if supported.
    shard_role_details::getRecoveryUnit(opCtx)->setNoEvictionAfterCommitOrRollback();

    while (true) {
        // Reset the value before killing operations as we only want to track the number
        // of operations that's running after step down.
        _totalOpsRunning = 0;
        _killConflictingOpsOnStepUpAndStepDown(killReason);

        // Destroy all stashed transaction resources, in order to release locks.
        SessionKiller::Matcher matcherAllSessions(
            KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
        killSessionsAbortUnpreparedTransactions(opCtx, matcherAllSessions, killReason);

        // Operations (like batch insert) that have currently yielded the global lock during step
        // down can reacquire global lock in IX mode when this node steps back up after a brief
        // network partition. And, this can lead to data inconsistency (see SERVER-27534). So,
        // its important we mark operations killed at least once after enqueuing the RSTL lock in
        // X mode for the first time. This ensures that no writing operations will continue
        // after the node's term change.
        {
            stdx::unique_lock lock(_mutex);
            if (_stopKillingOps.wait_for(
                    lock, Milliseconds(10).toSystemDuration(), [this] { return _killSignaled; })) {
                LOGV2(21344, "Stopped killing user operations");
                _stepUpStepDownCoord->updateAndLogStateTransitionMetrics(
                    _stateTransition, getTotalOpsKilled(), getTotalOpsRunning());
                _killSignaled = false;
                return;
            }
        }
    }
}

void AutoGetRstlForStepUpStepDown::_stopAndWaitForKillOpThread() {
    if (!(_killOpThread && _killOpThread->joinable()))
        return;

    {
        stdx::unique_lock lock(_mutex);
        _killSignaled = true;
        _stopKillingOps.notify_all();
    }
    _killOpThread->join();
    _killOpThread.reset();
}

size_t AutoGetRstlForStepUpStepDown::getTotalOpsKilled() const {
    return _totalOpsKilled;
}

void AutoGetRstlForStepUpStepDown::incrementTotalOpsKilled(size_t val) {
    _totalOpsKilled += val;
}

size_t AutoGetRstlForStepUpStepDown::getTotalOpsRunning() const {
    return _totalOpsRunning;
}

void AutoGetRstlForStepUpStepDown::incrementTotalOpsRunning(size_t val) {
    _totalOpsRunning += val;
}

void AutoGetRstlForStepUpStepDown::rstlRelease() {
    _rstlLock->release();
}

void AutoGetRstlForStepUpStepDown::rstlReacquire() {
    // Ensure that we are not holding the RSTL lock in any mode.
    invariant(!shard_role_details::getLocker(_opCtx)->isRSTLLocked() ||
              gFeatureFlagIntentRegistration.isEnabled());

    // Since we have released the RSTL lock at this point, there can be some conflicting
    // operations sneaked in here. We need to kill those operations to acquire the RSTL lock.
    // Also, its ok to start "RstlKillOpthread" thread before RSTL lock enqueue as we kill
    // operations in a loop.
    ON_BLOCK_EXIT([&] { _stopAndWaitForKillOpThread(); });
    _startKillOpThread();
    _rstlLock->reacquire();
}

void AutoGetRstlForStepUpStepDown::_killConflictingOpsOnStepUpAndStepDown(
    ErrorCodes::Error reason) {
    const OperationContext* rstlOpCtx = getOpCtx();
    ServiceContext* serviceCtx = rstlOpCtx->getServiceContext();
    invariant(serviceCtx);

    for (ServiceContext::LockedClientsCursor cursor(serviceCtx); Client* client = cursor.next();) {
        ClientLock lk(client);
        OperationContext* toKill = client->getOperationContext();

        // Don't kill step up/step down thread.
        if (toKill && !toKill->isKillPending() && toKill->getOpID() != rstlOpCtx->getOpID()) {
            auto& tracker = StorageExecutionContext::get(toKill)->getPrepareConflictTracker();
            bool isWaitingOnPrepareConflict = tracker.isWaitingOnPrepareConflict();
            if (client->canKillOperationInStepdown()) {
                auto locker = shard_role_details::getLocker(toKill);
                bool alwaysInterrupt = toKill->shouldAlwaysInterruptAtStepDownOrUp();
                bool globalLockConfict = locker->wasGlobalLockTakenInModeConflictingWithWrites();
                if (alwaysInterrupt || globalLockConfict || isWaitingOnPrepareConflict) {
                    serviceCtx->killOperation(lk, toKill, reason);
                    incrementTotalOpsKilled();
                    LOGV2(8562701,
                          "Repl state change interrupted a thread.",
                          "name"_attr = client->desc(),
                          "alwaysInterrupt"_attr = alwaysInterrupt,
                          "globalLockConflict"_attr = globalLockConfict,
                          "isWaitingOnPrepareConflict"_attr = isWaitingOnPrepareConflict);
                } else {
                    incrementTotalOpsRunning();
                }
            } else if (isWaitingOnPrepareConflict) {
                // All operations that hit a prepare conflict should be killable to prevent
                // deadlocks with prepared transactions on replica set step up and step down.
                LOGV2_FATAL(9699100,
                            "Repl state change encountered a non-killable thread blocked on a "
                            "prepare conflict.",
                            "name"_attr = client->desc(),
                            "conflictCount"_attr = tracker.getThisOpPrepareConflictCount(),
                            "conflictDurationMicros"_attr =
                                tracker.getThisOpPrepareConflictDuration());
            }
        }
    }
}

const OperationContext* AutoGetRstlForStepUpStepDown::getOpCtx() const {
    return _opCtx;
}

}  // namespace repl
}  // namespace mongo
