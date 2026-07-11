// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_yield_policy.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/yieldable.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(setPreYieldWait);
MONGO_FAIL_POINT_DEFINE(setPreYieldWaitDeferred);
}  // namespace

PlanYieldPolicy::PlanYieldPolicy(OperationContext* opCtx,
                                 YieldPolicy policy,
                                 ClockSource* cs,
                                 int yieldIterations,
                                 Milliseconds yieldPeriod,
                                 std::unique_ptr<const YieldPolicyCallbacks> callbacks)
    : _policy(getPolicyOverrideForOperation(opCtx, policy)),
      _callbacks(std::move(callbacks)),
      _elapsedTracker(cs, yieldIterations, yieldPeriod),
      _fastClock(&opCtx->fastClockSource()) {
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
    tassert(11321328, "opCtx must not be null", opCtx);
    setPreYieldWait.executeIf(
        [&](const BSONObj& data) {
            if (auto e = data["waitForMillis"]; !e.eoo()) {
                sleepFor(Milliseconds(e.numberInt()));
            }
            // Pause after setPreYieldWaitDeferred's skip count is exhausted.
            setPreYieldWaitDeferred.pauseWhileSet(opCtx);
        },
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

    for (int attempt = 1; true; attempt++) {
        try {
            saveState(opCtx);

            // TODO SERVER-103267: Remove setAbandonSnapshotMode() and related.

            if (getPolicy() == PlanYieldPolicy::YieldPolicy::WRITE_CONFLICT_RETRY_ONLY) {
                // This yield policy doesn't release locks, but it does relinquish our storage
                // snapshot.
                tassert(11321329,
                        "Invalid use of YieldPolicy::WRITE_CONFLICT_RETRY_ONLY on lock free reads",
                        !opCtx->isLockFreeReadsOp());
                shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
                if (afterSnapshotAbandonFn) {
                    afterSnapshotAbandonFn();
                }
            } else {
                performYieldWithAcquisitions(opCtx, whileYieldingFn, afterSnapshotAbandonFn);
            }

            restoreState(opCtx, nullptr, restoreType);
            return Status::OK();
        } catch (const StorageUnavailableException& e) {
            // Restore '_yieldable' before the retry.
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

void PlanYieldPolicy::performYieldWithAcquisitions(OperationContext* opCtx,
                                                   std::function<void()> whileYieldingFn,
                                                   std::function<void()> afterSnapshotAbandonFn) {
    // Things have to happen here in a specific order:
    //   * Remove all references to the CollectionPtrs to avoid holding stale references.
    //   * Abandon the current storage engine snapshot.
    //   * Check for interrupt if the yield policy requires.
    //   * Yield the acquired TransactionResources
    //   * Restore the yielded TransactionResources
    tassert(1321331,
            fmt::format("Unexpected yield policy {} during performYieldWithAcquisitions()",
                        serializeYieldPolicy(_policy)),
            canReleaseLocksDuringExecution());

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
