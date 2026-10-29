// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_yield_policy_release_memory.h"

#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/yield_policy_callbacks_impl.h"

namespace mongo {

PlanYieldPolicyReleaseMemory::PlanYieldPolicyReleaseMemory(
    OperationContext* opCtx,
    PlanYieldPolicy::YieldPolicy policy,
    std::unique_ptr<YieldPolicyCallbacks> callbacks)
    : PlanYieldPolicy(opCtx,
                      policy,
                      &opCtx->fastClockSource(),
                      internalQueryExecYieldIterations.load(),
                      Milliseconds{internalQueryExecYieldPeriodMS.load()},
                      std::move(callbacks)) {}

std::unique_ptr<PlanYieldPolicyReleaseMemory> PlanYieldPolicyReleaseMemory::make(
    OperationContext* opCtx, PlanYieldPolicy::YieldPolicy policy, NamespaceString nss) {
    return std::make_unique<PlanYieldPolicyReleaseMemory>(
        opCtx, policy, std::make_unique<YieldPolicyCallbacksImpl>(std::move(nss)));
}

void PlanYieldPolicyReleaseMemory::saveState(OperationContext* opCtx) {}

void PlanYieldPolicyReleaseMemory::restoreState(OperationContext* opCtx,
                                                const Yieldable* yieldable,
                                                RestoreContext::RestoreType restoreType) {}

}  // namespace mongo
