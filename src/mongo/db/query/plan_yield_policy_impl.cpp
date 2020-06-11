/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/query/plan_yield_policy_impl.h"

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/snapshot_helper.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(setInterruptOnlyPlansCheckForInterruptHang);
}  // namespace

PlanYieldPolicyImpl::PlanYieldPolicyImpl(PlanExecutor* exec, PlanYieldPolicy::YieldPolicy policy)
    : PlanYieldPolicy(exec->getOpCtx()->lockState()->isGlobalLockedRecursively()
                          ? PlanYieldPolicy::YieldPolicy::NO_YIELD
                          : policy,
                      exec->getOpCtx()->getServiceContext()->getFastClockSource(),
                      internalQueryExecYieldIterations.load(),
                      Milliseconds{internalQueryExecYieldPeriodMS.load()}),
      _planYielding(exec) {}

Status PlanYieldPolicyImpl::yield(OperationContext* opCtx, std::function<void()> whileYieldingFn) {
    // Can't use writeConflictRetry since we need to call saveState before reseting the
    // transaction.
    for (int attempt = 1; true; attempt++) {
        try {
            try {
                _planYielding->saveState();
            } catch (const WriteConflictException&) {
                invariant(!"WriteConflictException not allowed in saveState");
            }

            if (getPolicy() == PlanYieldPolicy::YieldPolicy::WRITE_CONFLICT_RETRY_ONLY) {
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
            // Errors other than write conflicts don't get retried, and should instead result in
            // the PlanExecutor dying. We propagate all such errors as status codes.
            return exceptionToStatus();
        }
    }
}

namespace {
MONGO_FAIL_POINT_DEFINE(setYieldAllLocksHang);
MONGO_FAIL_POINT_DEFINE(setYieldAllLocksWait);
}  // namespace

void PlanYieldPolicyImpl::_yieldAllLocks(OperationContext* opCtx,
                                         std::function<void()> whileYieldingFn,
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
    if (getPolicy() == PlanYieldPolicy::YieldPolicy::YIELD_AUTO) {
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

    setYieldAllLocksHang.executeIf(
        [opCtx](const BSONObj& config) {
            setYieldAllLocksHang.pauseWhileSet();

            if (config.getField("checkForInterruptAfterHang").trueValue()) {
                // Throws.
                opCtx->checkForInterrupt();
            }
        },
        [&](const BSONObj& config) {
            StringData ns = config.getStringField("namespace");
            return ns.empty() || ns == planExecNS.ns();
        });
    setYieldAllLocksWait.executeIf(
        [&](const BSONObj& data) { sleepFor(Milliseconds(data["waitForMillis"].numberInt())); },
        [&](const BSONObj& config) {
            BSONElement dataNs = config["namespace"];
            return !dataNs || planExecNS.ns() == dataNs.str();
        });

    if (whileYieldingFn) {
        whileYieldingFn();
    }

    locker->restoreLockState(opCtx, snapshot);

    // After yielding and reacquiring locks, the preconditions that were used to select our
    // ReadSource initially need to be checked again. Queries hold an AutoGetCollectionForRead RAII
    // lock for their lifetime, which may select a ReadSource based on state (e.g. replication
    // state). After a query yields its locks, this state may have changed, invalidating our current
    // choice of ReadSource. Using the same preconditions, change our ReadSource if necessary.
    auto newReadSource = SnapshotHelper::getNewReadSource(opCtx, planExecNS);
    if (newReadSource) {
        opCtx->recoveryUnit()->setTimestampReadSource(*newReadSource);
    }
}

void PlanYieldPolicyImpl::preCheckInterruptOnly(OperationContext* opCtx) {
    // If the 'setInterruptOnlyPlansCheckForInterruptHang' fail point is enabled, set the
    // 'failPointMsg' field of this operation's CurOp to signal that we've hit this point.
    if (MONGO_unlikely(setInterruptOnlyPlansCheckForInterruptHang.shouldFail())) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &setInterruptOnlyPlansCheckForInterruptHang,
            opCtx,
            "setInterruptOnlyPlansCheckForInterruptHang");
    }
}

}  // namespace mongo
