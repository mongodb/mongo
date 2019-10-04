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

#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_yield_policy.h"

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(setInterruptOnlyPlansCheckForInterruptHang);
}  // namespace

PlanYieldPolicy::PlanYieldPolicy(PlanExecutor* exec, PlanExecutor::YieldPolicy policy)
    : _policy(exec->getOpCtx()->lockState()->isGlobalLockedRecursively() ? PlanExecutor::NO_YIELD
                                                                         : policy),
      _forceYield(false),
      _elapsedTracker(exec->getOpCtx()->getServiceContext()->getFastClockSource(),
                      internalQueryExecYieldIterations.load(),
                      Milliseconds(internalQueryExecYieldPeriodMS.load())),
      _planYielding(exec) {}


PlanYieldPolicy::PlanYieldPolicy(PlanExecutor::YieldPolicy policy, ClockSource* cs)
    : _policy(policy),
      _forceYield(false),
      _elapsedTracker(cs,
                      internalQueryExecYieldIterations.load(),
                      Milliseconds(internalQueryExecYieldPeriodMS.load())),
      _planYielding(nullptr) {}

bool PlanYieldPolicy::shouldYieldOrInterrupt() {
    if (_policy == PlanExecutor::INTERRUPT_ONLY) {
        return _elapsedTracker.intervalHasElapsed();
    }
    if (!canAutoYield())
        return false;
    invariant(!_planYielding->getOpCtx()->lockState()->inAWriteUnitOfWork());
    if (_forceYield)
        return true;
    return _elapsedTracker.intervalHasElapsed();
}

void PlanYieldPolicy::resetTimer() {
    _elapsedTracker.resetLastTime();
}

Status PlanYieldPolicy::yieldOrInterrupt(stdx::function<void()> whileYieldingFn) {
    invariant(_planYielding);

    if (_policy == PlanExecutor::INTERRUPT_ONLY) {
        ON_BLOCK_EXIT([this]() { resetTimer(); });
        OperationContext* opCtx = _planYielding->getOpCtx();
        invariant(opCtx);
        // If the 'setInterruptOnlyPlansCheckForInterruptHang' fail point is enabled, set the 'msg'
        // field of this operation's CurOp to signal that we've hit this point.
        if (MONGO_FAIL_POINT(setInterruptOnlyPlansCheckForInterruptHang)) {
            CurOpFailpointHelpers::waitWhileFailPointEnabled(
                &setInterruptOnlyPlansCheckForInterruptHang,
                opCtx,
                "setInterruptOnlyPlansCheckForInterruptHang");
        }

        return opCtx->checkForInterruptNoAssert();
    }

    invariant(canAutoYield());

    // After we finish yielding (or in any early return), call resetTimer() to prevent yielding
    // again right away. We delay the resetTimer() call so that the clock doesn't start ticking
    // until after we return from the yield.
    ON_BLOCK_EXIT([this]() { resetTimer(); });

    _forceYield = false;

    OperationContext* opCtx = _planYielding->getOpCtx();
    invariant(opCtx);
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // Can't use writeConflictRetry since we need to call saveState before reseting the transaction.
    for (int attempt = 1; true; attempt++) {
        try {
            try {
                _planYielding->saveState();
            } catch (const WriteConflictException&) {
                invariant(!"WriteConflictException not allowed in saveState");
            }

            if (_policy == PlanExecutor::WRITE_CONFLICT_RETRY_ONLY) {
                // Just reset the snapshot. Leave all LockManager locks alone.
                opCtx->recoveryUnit()->abandonSnapshot();
            } else {
                // Release and reacquire locks.
                _yieldAllLocks(opCtx, whileYieldingFn, _planYielding->nss());
            }

            _planYielding->restoreStateWithoutRetrying();
            return Status::OK();
        } catch (const WriteConflictException&) {
            CurOp::get(opCtx)->debug().additiveMetrics.incrementWriteConflicts(1);
            WriteConflictException::logAndBackoff(
                attempt, "plan execution restoreState", _planYielding->nss().ns());
            // retry
        } catch (...) {
            // Errors other than write conflicts don't get retried, and should instead result in the
            // PlanExecutor dying. We propagate all such errors as status codes.
            return exceptionToStatus();
        }
    }
}

namespace {
MONGO_FAIL_POINT_DEFINE(setYieldAllLocksHang);
MONGO_FAIL_POINT_DEFINE(setYieldAllLocksWait);
}  // namespace

void PlanYieldPolicy::_yieldAllLocks(OperationContext* opCtx,
                                     stdx::function<void()> whileYieldingFn,
                                     const NamespaceString& planExecNS) {
    // Things have to happen here in a specific order:
    //   * Release lock mgr locks
    //   * Check for interrupt (kill flag is set)
    //   * Call the whileYieldingFn
    //   * Reacquire lock mgr locks

    Locker* locker = opCtx->lockState();

    Locker::LockSnapshot snapshot;

    auto unlocked = locker->saveLockStateAndUnlock(&snapshot);

    // Attempt to check for interrupt while locks are not held, in order to discourage the
    // assumption that locks will always be held when a Plan Executor returns an error.
    if (_policy == PlanExecutor::YIELD_AUTO) {
        opCtx->checkForInterrupt();  // throws
    }

    if (!unlocked) {
        // Nothing was unlocked, just return, yielding is pointless.
        return;
    }

    // Top-level locks are freed, release any potential low-level (storage engine-specific
    // locks). If we are yielding, we are at a safe place to do so.
    opCtx->recoveryUnit()->abandonSnapshot();

    // Track the number of yields in CurOp.
    CurOp::get(opCtx)->yielded();

    MONGO_FAIL_POINT_BLOCK(setYieldAllLocksHang, config) {
        StringData ns{config.getData().getStringField("namespace")};
        if (ns.empty() || ns == planExecNS.ns()) {
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(setYieldAllLocksHang);
        }

        if (config.getData().getField("checkForInterruptAfterHang").trueValue()) {
            // Throws.
            opCtx->checkForInterrupt();
        }
    }

    MONGO_FAIL_POINT_BLOCK(setYieldAllLocksWait, customWait) {
        const BSONObj& data = customWait.getData();
        BSONElement customWaitNS = data["namespace"];
        if (!customWaitNS || planExecNS.ns() == customWaitNS.str()) {
            sleepFor(Milliseconds(data["waitForMillis"].numberInt()));
        }
    }

    if (whileYieldingFn) {
        whileYieldingFn();
    }

    locker->restoreLockState(opCtx, snapshot);
}

}  // namespace mongo
