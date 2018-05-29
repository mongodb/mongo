/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_yield_policy.h"

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_yield.h"
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
    return shouldYield();
}

bool PlanYieldPolicy::shouldYield() {
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

Status PlanYieldPolicy::yieldOrInterrupt(RecordFetcher* recordFetcher) {
    invariant(_planYielding);
    if (recordFetcher) {
        OperationContext* opCtx = _planYielding->getOpCtx();
        return yieldOrInterrupt([recordFetcher, opCtx] { recordFetcher->setup(opCtx); },
                                [recordFetcher] { recordFetcher->fetch(); });
    } else {
        return yieldOrInterrupt(nullptr, nullptr);
    }
}

Status PlanYieldPolicy::yieldOrInterrupt(stdx::function<void()> beforeYieldingFn,
                                         stdx::function<void()> whileYieldingFn) {
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

    return yield(beforeYieldingFn, whileYieldingFn);
}

Status PlanYieldPolicy::yield(stdx::function<void()> beforeYieldingFn,
                              stdx::function<void()> whileYieldingFn) {
    invariant(_planYielding);
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
            // All YIELD_AUTO plans will get here eventually when the elapsed tracker triggers
            // that it's time to yield. Whether or not we will actually yield, we need to check
            // if this operation has been interrupted.
            if (_policy == PlanExecutor::YIELD_AUTO) {
                auto interruptStatus = opCtx->checkForInterruptNoAssert();
                if (!interruptStatus.isOK()) {
                    return interruptStatus;
                }
            }

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
                if (beforeYieldingFn)
                    beforeYieldingFn();
                QueryYield::yieldAllLocks(opCtx, whileYieldingFn, _planYielding->nss());
            }

            return _planYielding->restoreStateWithoutRetrying();
        } catch (const WriteConflictException&) {
            CurOp::get(opCtx)->debug().writeConflicts++;
            WriteConflictException::logAndBackoff(
                attempt, "plan execution restoreState", _planYielding->nss().ns());
            // retry
        }
    }
}

}  // namespace mongo
