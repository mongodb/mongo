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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {

PlanYieldPolicy::PlanYieldPolicy(
    OperationContext* opCtx,
    YieldPolicy policy,
    ClockSource* cs,
    int yieldIterations,
    Milliseconds yieldPeriod,
    stdx::variant<const Yieldable*, YieldThroughAcquisitions> yieldable,
    std::unique_ptr<const YieldPolicyCallbacks> callbacks)
    : _policy(getPolicyOverrideForOperation(opCtx, policy)),
      _yieldable(yieldable),
      _callbacks(std::move(callbacks)),
      _elapsedTracker(cs, yieldIterations, yieldPeriod) {
    stdx::visit(OverloadedVisitor{[&](const Yieldable* collectionPtr) {
                                      invariant(!collectionPtr || collectionPtr->yieldable() ||
                                                policy == YieldPolicy::WRITE_CONFLICT_RETRY_ONLY ||
                                                policy == YieldPolicy::NO_YIELD ||
                                                policy == YieldPolicy::INTERRUPT_ONLY ||
                                                policy == YieldPolicy::ALWAYS_TIME_OUT ||
                                                policy == YieldPolicy::ALWAYS_MARK_KILLED);
                                  },
                                  [&](const YieldThroughAcquisitions& yieldThroughAcquisitions) {
                                      // CollectionAcquisitions are always yieldable.
                                  }},
                _yieldable);
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
    // operation that should not be interrupted or yielded.
    // TODO: SERVER-76238 Evaluate if we can make everything INTERRUPT_ONLY instead.
    if (!opCtx->lockState()->canSaveLockState() &&
        (desired == YieldPolicy::YIELD_AUTO || desired == YieldPolicy::YIELD_MANUAL)) {
        return YieldPolicy::NO_YIELD;
    }

    return desired;
}

bool PlanYieldPolicy::shouldYieldOrInterrupt(OperationContext* opCtx) {
    if (_policy == YieldPolicy::INTERRUPT_ONLY) {
        return _elapsedTracker.intervalHasElapsed();
    }
    if (!canAutoYield())
        return false;
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());
    if (_forceYield)
        return true;
    return _elapsedTracker.intervalHasElapsed();
}

void PlanYieldPolicy::resetTimer() {
    _elapsedTracker.resetLastTime();
}

Status PlanYieldPolicy::yieldOrInterrupt(OperationContext* opCtx,
                                         std::function<void()> whileYieldingFn) {
    invariant(opCtx);

    if (_policy == YieldPolicy::INTERRUPT_ONLY) {
        ON_BLOCK_EXIT([this]() { resetTimer(); });
        invariant(opCtx);
        if (_callbacks) {
            _callbacks->preCheckInterruptOnly(opCtx);
        }
        return opCtx->checkForInterruptNoAssert();
    }

    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // After we finish yielding (or in any early return), call resetTimer() to prevent yielding
    // again right away. We delay the resetTimer() call so that the clock doesn't start ticking
    // until after we return from the yield.
    ON_BLOCK_EXIT([this]() { resetTimer(); });
    _forceYield = false;

    for (int attempt = 1; true; attempt++) {
        try {
            // Saving and restoring can modify '_yieldable', so we make a copy before we start.
            const auto yieldable = _yieldable;

            try {
                saveState(opCtx);
            } catch (const WriteConflictException&) {
                // Saving the state of an execution plan must never throw WCE.
                MONGO_UNREACHABLE;
            }

            boost::optional<ScopeGuard<std::function<void()>>> exitGuard;
            if (useExperimentalCommitTxnBehavior()) {
                // All data pointed to by cursors must remain valid across the yield. Setting this
                // flag for the duration of yield will force any calls to abandonSnapshot() to
                // commit the transaction, rather than abort it, in order to leave the cursors
                // valid.
                opCtx->recoveryUnit()->setAbandonSnapshotMode(
                    RecoveryUnit::AbandonSnapshotMode::kCommit);
                exitGuard.emplace([&] {
                    invariant(opCtx->recoveryUnit()->abandonSnapshotMode() ==
                              RecoveryUnit::AbandonSnapshotMode::kCommit);
                    opCtx->recoveryUnit()->setAbandonSnapshotMode(
                        RecoveryUnit::AbandonSnapshotMode::kAbort);
                });
            }

            if (getPolicy() == PlanYieldPolicy::YieldPolicy::WRITE_CONFLICT_RETRY_ONLY) {
                // This yield policy doesn't release locks, but it does relinquish our storage
                // snapshot.
                invariant(!opCtx->isLockFreeReadsOp());
                opCtx->recoveryUnit()->abandonSnapshot();
            } else {
                if (usesCollectionAcquisitions()) {
                    performYieldWithAcquisitions(opCtx, whileYieldingFn);
                } else {
                    const Yieldable* yieldablePtr = stdx::get<const Yieldable*>(yieldable);
                    invariant(yieldablePtr);
                    performYield(opCtx, *yieldablePtr, whileYieldingFn);
                }
            }

            restoreState(opCtx,
                         stdx::holds_alternative<const Yieldable*>(yieldable)
                             ? stdx::get<const Yieldable*>(yieldable)
                             : nullptr);
            return Status::OK();
        } catch (const WriteConflictException&) {
            if (_callbacks) {
                _callbacks->handledWriteConflict(opCtx);
            }
            logWriteConflictAndBackoff(attempt, "query yield", ""_sd);
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
                                   std::function<void()> whileYieldingFn) {
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
    opCtx->recoveryUnit()->abandonSnapshot();

    // Check for interrupt before releasing locks. This avoids the complexities of having to
    // re-acquire locks to clean up when we are interrupted. This is the main interrupt check during
    // query execution. Yield points and interrupt points are one and the same.
    if (getPolicy() == PlanYieldPolicy::YieldPolicy::YIELD_AUTO) {
        opCtx->checkForInterrupt();  // throws
    }

    Locker* locker = opCtx->lockState();
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
                                                   std::function<void()> whileYieldingFn) {
    // Things have to happen here in a specific order:
    //   * Yield the acquired TransactionResources
    //   * Abandon the current storage engine snapshot.
    //   * Check for interrupt if the yield policy requires.
    //   * Restore the yielded TransactionResources
    invariant(_policy == YieldPolicy::YIELD_AUTO || _policy == YieldPolicy::YIELD_MANUAL);

    // Release any storage engine resources. This requires holding a global lock to correctly
    // synchronize with states such as shutdown and rollback.
    opCtx->recoveryUnit()->abandonSnapshot();

    // Check for interrupt before releasing locks. This avoids the complexities of having to
    // re-acquire locks to clean up when we are interrupted. This is the main interrupt check during
    // query execution. Yield points and interrupt points are one and the same.
    if (getPolicy() == PlanYieldPolicy::YieldPolicy::YIELD_AUTO) {
        opCtx->checkForInterrupt();  // throws
    }

    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx);
    ScopeGuard disposeYieldedTransactionResourcesScopeGuard([&yieldedTransactionResources] {
        if (yieldedTransactionResources) {
            yieldedTransactionResources->dispose();
        }
    });

    if (!yieldedTransactionResources) {
        // Nothing was unlocked. Recursively held locks are not the only reason locks cannot be
        // released.
        return;
    }

    if (_callbacks) {
        _callbacks->duringYield(opCtx);
    }

    if (whileYieldingFn) {
        whileYieldingFn();
    }

    disposeYieldedTransactionResourcesScopeGuard.dismiss();
    try {
        restoreTransactionResourcesToOperationContext(opCtx,
                                                      std::move(*yieldedTransactionResources));
    } catch (const ExceptionFor<ErrorCodes::CollectionUUIDMismatch>& ex) {
        const auto extraInfo = ex.extraInfo<CollectionUUIDMismatchInfo>();
        if (extraInfo->actualCollection()) {
            throwCollectionRenamedError(NamespaceString(extraInfo->expectedCollection()),
                                        NamespaceString(*extraInfo->actualCollection()),
                                        extraInfo->collectionUUID());
        } else {
            throwCollectionDroppedError(extraInfo->collectionUUID());
        }
    }
}

}  // namespace mongo
