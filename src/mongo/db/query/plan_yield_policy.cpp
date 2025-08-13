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

#include "mongo/db/query/plan_yield_policy.h"

#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/yieldable.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(setPreYieldWait);
}  // namespace

PlanYieldPolicy::PlanYieldPolicy(OperationContext* opCtx,
                                 YieldPolicy policy,
                                 ClockSource* cs,
                                 int yieldIterations,
                                 Milliseconds yieldPeriod,
                                 std::variant<const Yieldable*, YieldThroughAcquisitions> yieldable,
                                 std::unique_ptr<const YieldPolicyCallbacks> callbacks)
    : _policy(getPolicyOverrideForOperation(opCtx, policy)),
      _yieldable(yieldable),
      _callbacks(std::move(callbacks)),
      _elapsedTracker(cs, yieldIterations, yieldPeriod),
      _fastClock(&opCtx->fastClockSource()) {
    visit(OverloadedVisitor{[&](const Yieldable* collectionPtr) {
                                invariant(!collectionPtr || collectionPtr->yieldable() ||
                                          policy == YieldPolicy::WRITE_CONFLICT_RETRY_ONLY ||
                                          policy == YieldPolicy::INTERRUPT_ONLY ||
                                          policy == YieldPolicy::ALWAYS_TIME_OUT ||
                                          policy == YieldPolicy::ALWAYS_MARK_KILLED);
                            },
                            [&](const YieldThroughAcquisitions& yieldThroughAcquisitions) {
                                // CollectionAcquisitions are always yieldable.
                            }},
          _yieldable);

    // If 'internalQueryExecYieldIterations' is the default value, we will only reply on time period
    // to check for yielding, so we can avoid do the check for every iteration instead do the check
    // every half of the period.
    _yieldIntervalMs = internalQueryExecYieldIterations.load() == -1
        ? internalQueryExecYieldPeriodMS.load() / 2
        : -1;
}

PlanYieldPolicy::YieldPolicy PlanYieldPolicy::getPolicyOverrideForOperation(
    OperationContext* opCtx, PlanYieldPolicy::YieldPolicy desired) {
    // We may have a null opCtx in testing.
    if (MONGO_unlikely(!opCtx)) {
        return desired;
    }
    // Multi-document transactions cannot yield locks or snapshots. We convert to a non-yielding
    // interruptible plan.
    if (opCtx->inMultiDocumentTransaction() &&
        (desired == YieldPolicy::YIELD_AUTO || desired == YieldPolicy::YIELD_MANUAL ||
         desired == YieldPolicy::WRITE_CONFLICT_RETRY_ONLY)) {
        return YieldPolicy::INTERRUPT_ONLY;
    }

    // If the state of our locks held is not yieldable at all, we will assume this is an internal
    // operation that will not yield.
    if (!shard_role_details::getLocker(opCtx)->canSaveLockState() &&
        (desired == YieldPolicy::YIELD_AUTO || desired == YieldPolicy::YIELD_MANUAL)) {
        return YieldPolicy::INTERRUPT_ONLY;
    }

    return desired;
}

bool PlanYieldPolicy::doShouldYieldOrInterrupt(OperationContext* opCtx) {
    if (_policy == YieldPolicy::INTERRUPT_ONLY) {
        return _elapsedTracker.intervalHasElapsed();
    }
    if (!canAutoYield())
        return false;
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
    if (_forceYield)
        return true;
    return _elapsedTracker.intervalHasElapsed();
}

void PlanYieldPolicy::resetTimer() {
    _elapsedTracker.resetLastTime();
}

Status PlanYieldPolicy::yieldOrInterrupt(OperationContext* opCtx,
                                         const std::function<void()>& whileYieldingFn,
                                         RestoreContext::RestoreType restoreType,
                                         const std::function<void()>& afterSnapshotAbandonFn) {
    invariant(opCtx);
    setPreYieldWait.executeIf(
        [&](const BSONObj& data) { sleepFor(Milliseconds(data["waitForMillis"].numberInt())); },
        [&](const BSONObj& config) {
            if (config.hasField("comment") && opCtx->getComment()) {
                return opCtx->getComment()->String() == config.getStringField("comment");
            }
            return false;
        });

    // After we finish yielding (or in any early return), call resetTimer() to prevent yielding
    // again right away. We delay the resetTimer() call so that the clock doesn't start ticking
    // until after we return from the yield.
    ON_BLOCK_EXIT([this]() { resetTimer(); });

    if (_policy == YieldPolicy::INTERRUPT_ONLY) {
        if (_callbacks) {
            _callbacks->preCheckInterruptOnly(opCtx);
        }
        return opCtx->checkForInterruptNoAssert();
    }

    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    _forceYield = false;

    // Saving and restoring can modify '_yieldable', so we make a copy before we start. This copying
    // cannot throw.
    const std::variant<const Yieldable*, YieldThroughAcquisitions> yieldable = _yieldable;
    for (int attempt = 1; true; attempt++) {
        try {
            // This sets _yieldable to a nullptr.
            saveState(opCtx);

            boost::optional<ScopeGuard<std::function<void()>>> exitGuard;

            // TODO SERVER-103267: Remove setAbandonSnapshotMode() and related.

            if (getPolicy() == PlanYieldPolicy::YieldPolicy::WRITE_CONFLICT_RETRY_ONLY) {
                // This yield policy doesn't release locks, but it does relinquish our storage
                // snapshot.
                invariant(!opCtx->isLockFreeReadsOp());
                shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
                if (afterSnapshotAbandonFn) {
                    afterSnapshotAbandonFn();
                }
            } else {
                if (usesCollectionAcquisitions()) {
                    performYieldWithAcquisitions(opCtx, whileYieldingFn, afterSnapshotAbandonFn);
                } else {
                    const Yieldable* yieldablePtr = get<const Yieldable*>(yieldable);
                    tassert(9762900,
                            str::stream()
                                << "no yieldable object available for yield policy "
                                << serializeYieldPolicy(getPolicy()) << " in attempt " << attempt,
                            yieldablePtr);
                    performYield(opCtx, *yieldablePtr, whileYieldingFn, afterSnapshotAbandonFn);
                }
            }

            // This copies 'yieldable's contents back to '_yieldable' where needed.
            auto yieldablePtr = get_if<const Yieldable*>(&yieldable);
            restoreState(opCtx, yieldablePtr ? *yieldablePtr : nullptr, restoreType);
            return Status::OK();
        } catch (const StorageUnavailableException& e) {
            // Restore '_yieldable' before the retry.
            _yieldable = yieldable;
            logAndRecordWriteConflictAndBackoff(opCtx,
                                                attempt,
                                                "query yield",
                                                e.reason(),
                                                NamespaceStringOrUUID(NamespaceString::kEmpty));
            // Retry the yielding process.
        } catch (...) {
            // Errors other than write conflicts don't get retried, and should instead result in
            // the PlanExecutor dying. We propagate all such errors as status codes.
            return exceptionToStatus();
        }
    }

    MONGO_UNREACHABLE;
}

void PlanYieldPolicy::performYield(OperationContext* opCtx,
                                   const Yieldable& yieldable,
                                   std::function<void()> whileYieldingFn,
                                   std::function<void()> afterSnapshotAbandonFn) {
    // Things have to happen here in a specific order:
    //   * Release 'yieldable'.
    //   * Abandon the current storage engine snapshot.
    //   * Check for interrupt if the yield policy requires.
    //   * Release lock manager locks.
    //   * Reacquire lock manager locks.
    //   * Restore 'yieldable'.
    invariant(_policy == YieldPolicy::YIELD_AUTO || _policy == YieldPolicy::YIELD_MANUAL);

    // If we are here, the caller has guaranteed locks are not recursively held. This is a top level
    // operation and we can safely clear the 'yieldable' state before unlocking and then
    // re-establish it after re-locking.
    yieldable.yield();

    // Release any storage engine resources. This requires holding a global lock to correctly
    // synchronize with states such as shutdown and rollback.
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

    // Check for interrupt before releasing locks. This avoids the complexities of having to
    // re-acquire locks to clean up when we are interrupted. This is the main interrupt check during
    // query execution. Yield points and interrupt points are one and the same.
    if (getPolicy() == PlanYieldPolicy::YieldPolicy::YIELD_AUTO) {
        opCtx->checkForInterrupt();  // throws
    }

    // After we've abandoned our snapshot, perform any work before releasing locks.
    if (afterSnapshotAbandonFn) {
        afterSnapshotAbandonFn();
    }

    Locker* locker = shard_role_details::getLocker(opCtx);
    Locker::LockSnapshot snapshot;
    locker->saveLockStateAndUnlock(&snapshot);

    if (_callbacks) {
        _callbacks->duringYield(opCtx);
    }

    if (whileYieldingFn) {
        whileYieldingFn();
    }

    locker->restoreLockState(opCtx, snapshot);

    // A yield has occurred, but there still may not be a 'yieldable' if the PlanExecutor
    // has a 'locks internally' lock policy.
    // Yieldable restore may set a new read source if necessary.
    yieldable.restore();
}

void PlanYieldPolicy::performYieldWithAcquisitions(OperationContext* opCtx,
                                                   std::function<void()> whileYieldingFn,
                                                   std::function<void()> afterSnapshotAbandonFn) {
    // Things have to happen here in a specific order:
    //   * Remove all references to the CollectionPtrs to avoid holding stale references.
    //   * Abandon the current storage engine snapshot.
    //   * Check for interrupt if the yield policy requires.
    //   * Yield the acquired TransactionResources
    //   * Restore the yielded TransactionResources
    invariant(_policy == YieldPolicy::YIELD_AUTO || _policy == YieldPolicy::YIELD_MANUAL);

    // Remove stale CollectionPtr references from the acquisitions.
    auto preparedForYieldToken = prepareForYieldingTransactionResources(opCtx);

    // Release any storage engine resources. This requires holding a global lock to correctly
    // synchronize with states such as shutdown and rollback.
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

    // Check for interrupt before releasing locks. This avoids the complexities of having to
    // re-acquire locks to clean up when we are interrupted. This is the main interrupt check during
    // query execution. Yield points and interrupt points are one and the same.
    if (getPolicy() == PlanYieldPolicy::YieldPolicy::YIELD_AUTO) {
        opCtx->checkForInterrupt();  // throws
    }

    // After we've abandoned our snapshot, perform any work before yielding transaction resources.
    if (afterSnapshotAbandonFn) {
        afterSnapshotAbandonFn();
    }

    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(opCtx, preparedForYieldToken);
    ScopeGuard yieldFailedScopeGuard(
        [&] { yieldedTransactionResources.transitionTransactionResourcesToFailedState(opCtx); });

    if (_callbacks) {
        _callbacks->duringYield(opCtx);
    }

    if (whileYieldingFn) {
        whileYieldingFn();
    }

    yieldFailedScopeGuard.dismiss();
    restoreTransactionResourcesToOperationContext(opCtx, std::move(yieldedTransactionResources));
}

}  // namespace mongo
