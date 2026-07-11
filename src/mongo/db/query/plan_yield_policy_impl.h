// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/yield_policy_callbacks_impl.h"
#include "mongo/db/yieldable.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class PlanYieldPolicyImpl final : public PlanYieldPolicy {
public:
    PlanYieldPolicyImpl(OperationContext* opCtx,
                        PlanExecutorImpl* exec,
                        PlanYieldPolicy::YieldPolicy policy,
                        std::unique_ptr<YieldPolicyCallbacks> callbacks);

private:
    void saveState(OperationContext* opCtx) override;

    void restoreState(OperationContext* opCtx,
                      const Yieldable* yieldable,
                      RestoreContext::RestoreType restoreType) override;

    // The plan executor which this yield policy is responsible for yielding. Must not outlive the
    // plan executor.
    PlanExecutorImpl* const _planYielding;
};

/**
 * The yield policy for classic multiplanning trial period.
 */
class PlanYieldPolicyClassicTrialPeriod final : public PlanYieldPolicy {
public:
    PlanYieldPolicyClassicTrialPeriod(OperationContext* opCtx,
                                      PlanStage* root,
                                      PlanYieldPolicy::YieldPolicy policy,
                                      std::unique_ptr<YieldPolicyCallbacks> callbacks);

private:
    void saveState(OperationContext* opCtx) override;

    void restoreState(OperationContext* opCtx,
                      const Yieldable* yieldable,
                      RestoreContext::RestoreType restoreType) override;

    PlanStage* const _root;
};

/**
 * Creates the yield policy based on the PlanType. If PlanType is PlanStage (i.e. we are planning
 * sbe stages using classic multiplanner), then the PolicyType is PlanYieldPolicyClassicTrialPeriod,
 * otherwise if we are using classic executor, the PolicyType is PlanYieldPolicyClassicExecutor.
 */
template <typename PlanType>
std::unique_ptr<PlanYieldPolicy> makeClassicYieldPolicy(OperationContext* opCtx,
                                                        NamespaceString nss,
                                                        PlanType* plan,
                                                        PlanYieldPolicy::YieldPolicy policy) {
    using PolicyType = std::conditional_t<std::is_same_v<PlanType, PlanStage>,
                                          PlanYieldPolicyClassicTrialPeriod,
                                          PlanYieldPolicyImpl>;

    switch (policy) {
        case PlanYieldPolicy::YieldPolicy::YIELD_AUTO:
        case PlanYieldPolicy::YieldPolicy::YIELD_MANUAL:
        case PlanYieldPolicy::YieldPolicy::WRITE_CONFLICT_RETRY_ONLY:
        case PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY: {
            return std::make_unique<PolicyType>(
                opCtx, plan, policy, std::make_unique<YieldPolicyCallbacksImpl>(std::move(nss)));
        }
        case PlanYieldPolicy::YieldPolicy::ALWAYS_TIME_OUT: {
            return std::make_unique<AlwaysTimeOutYieldPolicy>(opCtx, &opCtx->fastClockSource());
        }
        case PlanYieldPolicy::YieldPolicy::ALWAYS_MARK_KILLED: {
            return std::make_unique<AlwaysPlanKilledYieldPolicy>(opCtx, &opCtx->fastClockSource());
        }
    }
    MONGO_UNREACHABLE;
}

}  // namespace mongo
