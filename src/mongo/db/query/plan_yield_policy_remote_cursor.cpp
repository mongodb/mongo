// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_yield_policy_remote_cursor.h"

#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/restore_context.h"
#include "mongo/db/query/yield_policy_callbacks_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/duration.h"

#include <utility>

namespace mongo {

std::unique_ptr<PlanYieldPolicyRemoteCursor> PlanYieldPolicyRemoteCursor::make(
    OperationContext* opCtx,
    PlanYieldPolicy::YieldPolicy policy,
    const MultipleCollectionAccessor& collections,
    NamespaceString nss,
    PlanExecutor* exec) {
    auto yieldPolicy = std::unique_ptr<PlanYieldPolicyRemoteCursor>(new PlanYieldPolicyRemoteCursor(
        opCtx, policy, std::make_unique<YieldPolicyCallbacksImpl>(std::move(nss)), exec));
    return yieldPolicy;
}

PlanYieldPolicyRemoteCursor::PlanYieldPolicyRemoteCursor(
    OperationContext* opCtx,
    PlanYieldPolicy::YieldPolicy policy,
    std::unique_ptr<YieldPolicyCallbacks> callbacks,
    PlanExecutor* exec)
    : PlanYieldPolicy(opCtx,
                      policy,
                      &opCtx->fastClockSource(),
                      internalQueryExecYieldIterations.load(),
                      Milliseconds{internalQueryExecYieldPeriodMS.load()},
                      std::move(callbacks)),
      _exec(exec) {}

void PlanYieldPolicyRemoteCursor::saveState(OperationContext* opCtx) {
    if (_exec) {
        _exec->saveState();
    }
}

void PlanYieldPolicyRemoteCursor::restoreState(OperationContext* opCtx,
                                               const Yieldable* yieldable,
                                               RestoreContext::RestoreType restoreType) {
    if (_exec) {
        // collPtr is expected to be null, if yieldable is not CollectionPtr.
        auto collPtr = dynamic_cast<const CollectionPtr*>(yieldable);
        _exec->restoreState({restoreType, collPtr});
    }
}

}  // namespace mongo
