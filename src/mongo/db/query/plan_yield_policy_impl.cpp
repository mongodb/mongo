// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_yield_policy_impl.h"

#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/restore_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/duration.h"

#include <utility>

namespace mongo {

PlanYieldPolicyImpl::PlanYieldPolicyImpl(OperationContext* opCtx,
                                         PlanExecutorImpl* exec,
                                         PlanYieldPolicy::YieldPolicy policy,
                                         std::unique_ptr<YieldPolicyCallbacks> callbacks)
    : PlanYieldPolicy(opCtx,
                      policy,
                      &opCtx->fastClockSource(),
                      internalQueryExecYieldIterations.load(),
                      Milliseconds{internalQueryExecYieldPeriodMS.load()},
                      std::move(callbacks)),
      _planYielding(exec) {}

void PlanYieldPolicyImpl::saveState(OperationContext* opCtx) {
    _planYielding->saveState();
}

void PlanYieldPolicyImpl::restoreState(OperationContext* opCtx,
                                       const Yieldable* yieldable,
                                       RestoreContext::RestoreType restoreType) {
    auto collectionPtr = checked_cast<const CollectionPtr*>(yieldable);
    _planYielding->restoreStateWithoutRetrying({restoreType, collectionPtr});
}


PlanYieldPolicyClassicTrialPeriod::PlanYieldPolicyClassicTrialPeriod(
    OperationContext* opCtx,
    PlanStage* root,
    PlanYieldPolicy::YieldPolicy policy,
    std::unique_ptr<YieldPolicyCallbacks> callbacks)
    : PlanYieldPolicy(opCtx,
                      policy,
                      &opCtx->fastClockSource(),
                      internalQueryExecYieldIterations.load(),
                      Milliseconds{internalQueryExecYieldPeriodMS.load()},
                      std::move(callbacks)),
      _root(root) {}

void PlanYieldPolicyClassicTrialPeriod::saveState(OperationContext* opCtx) {
    _root->saveState();
}

void PlanYieldPolicyClassicTrialPeriod::restoreState(OperationContext* opCtx,
                                                     const Yieldable* yieldable,
                                                     RestoreContext::RestoreType restoreType) {
    _root->restoreState({restoreType, dynamic_cast<const CollectionPtr*>(yieldable)});
}

}  // namespace mongo
