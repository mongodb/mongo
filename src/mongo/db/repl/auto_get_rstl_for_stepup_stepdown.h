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

#pragma once

#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/replication_state_transition_lock_guard.h"
#include "mongo/db/session/kill_sessions_local.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {
class StepUpStepDownCoordinator {
public:
    /**
     * Called by AutoGetRstlForStepUpStepDown before taking RSTL when making stepdown transitions
     */
    virtual void autoGetRstlEnterStepDown() = 0;

    /**
     * Called by AutoGetRstlForStepUpStepDown before releasing RSTL when making stepdown
     * transitions.  Also called in case of failure to acquire RSTL.  There will be one call to this
     * method for each call to autoGetRSTLEnterStepDown.
     */
    virtual void autoGetRstlExitStepDown() = 0;

    /**
     * Called by AutoGetRstlForStepUpStepDown after step up or stepdown to record metrics.
     */
    virtual void updateAndLogStateTransitionMetrics(
        ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
        size_t numOpsKilled,
        size_t numOpsRunning) const = 0;
};

// This object acquires RSTL in X mode to perform state transition to (step up)/from (step down)
// primary. In order to acquire RSTL, it also starts "RstlKillOpthread" which kills conflicting
// operations (user/system) and aborts stashed running transactions.
class AutoGetRstlForStepUpStepDown {
public:
    AutoGetRstlForStepUpStepDown(
        StepUpStepDownCoordinator* repl,
        OperationContext* opCtx,
        ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
        Date_t deadline = Date_t::max());

    ~AutoGetRstlForStepUpStepDown();

    // Disallows copying.
    AutoGetRstlForStepUpStepDown(const AutoGetRstlForStepUpStepDown&) = delete;
    AutoGetRstlForStepUpStepDown& operator=(const AutoGetRstlForStepUpStepDown&) = delete;

    /*
     * Releases RSTL lock.
     */
    void rstlRelease();

    /*
     * Reacquires RSTL lock.
     */
    void rstlReacquire();

    /*
     * Returns _totalOpsKilled value.
     */
    size_t getTotalOpsKilled() const;

    /*
     * Increments _totalOpsKilled by val.
     */
    void incrementTotalOpsKilled(size_t val = 1);

    /*
     * Returns _totalOpsRunning value.
     */
    size_t getTotalOpsRunning() const;

    /*
     * Increments _totalOpsRunning by val.
     */
    void incrementTotalOpsRunning(size_t val = 1);

    /*
     * Returns the step up/step down opCtx.
     */
    const OperationContext* getOpCtx() const;

private:
    /**
     * It will spawn a new thread killOpThread to kill operations that conflict with state
     * transitions (step up and step down).
     */
    void _startKillOpThread();

    /**
     * On state transition, we need to kill all write operations and all transactional
     * operations, so that unprepared and prepared transactions can release or yield their
     * locks. The required ordering between step up/step down steps are:
     * 1) Enqueue RSTL in X mode.
     * 2) Kill all conflicting operations.
     *       - Write operation that takes global lock in IX and X mode.
     *       - Read operations that takes global lock in S mode.
     *       - Operations(read/write) that are blocked on prepare conflict.
     * 3) Abort unprepared transactions.
     * 4) Repeat step 2) and 3) until the step up/step down thread can acquire RSTL.
     * 5) Yield locks of all prepared transactions. This applies only to step down as on
     * secondary we currently yield locks for prepared transactions.
     *
     * Since prepared transactions don't hold RSTL, step 1) to step 3) make sure all
     * running transactions that may hold RSTL finish, get killed or yield their locks,
     * so that we can acquire RSTL at step 4). Holding the locks of prepared transactions
     * until step 5) guarantees if any conflict operations (e.g. DDL operations) failed
     * to be killed for any reason, we will get a deadlock instead of a silent data corruption.
     *
     * Loops continuously to kill all conflicting operations. And, aborts all stashed (inactive)
     * transactions.
     * Terminates once killSignaled is set true.
     */
    void _killOpThreadFn();

    /*
     * Signals killOpThread to stop killing operations.
     */
    void _stopAndWaitForKillOpThread();

    /**
     * kill all conflicting operations that are blocked either on prepare conflict or have taken
     * global lock not in MODE_IS. The conflicting operations can be either user or system
     * operations marked as killable.
     */
    void _killConflictingOpsOnStepUpAndStepDown(ErrorCodes::Error reason);

    StepUpStepDownCoordinator* const _stepUpStepDownCoord;  // not owned.
    // step up/step down opCtx.
    OperationContext* const _opCtx;  // not owned.
    // This field is optional because we need to start killOpThread to kill operations after
    // RSTL enqueue.
    boost::optional<ReplicationStateTransitionLockGuard> _rstlLock;
    // Thread that will run killOpThreadFn().
    std::unique_ptr<stdx::thread> _killOpThread;
    // Tracks number of operations killed on step up / step down.
    size_t _totalOpsKilled = 0;
    // Tracks number of operations left running on step up / step down.
    size_t _totalOpsRunning = 0;
    // Protects killSignaled and stopKillingOps cond. variable.
    stdx::mutex _mutex;
    // Signals thread about the change of killSignaled value.
    stdx::condition_variable _stopKillingOps;
    // Once this is set to true, the killOpThreadFn method will terminate.
    bool _killSignaled = false;
    // The state transition that is in progress. Should never be set to rollback within this
    // class.
    ReplicationCoordinator::OpsKillingStateTransitionEnum _stateTransition;
};
}  // namespace repl
}  // namespace mongo
