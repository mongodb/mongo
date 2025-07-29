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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/yield_policy_callbacks_impl.h"
#include "mongo/db/yieldable.h"

#include <memory>
#include <variant>

namespace mongo {

class PlanYieldPolicyImpl final : public PlanYieldPolicy {
public:
    PlanYieldPolicyImpl(OperationContext* opCtx,
                        PlanExecutorImpl* exec,
                        PlanYieldPolicy::YieldPolicy policy,
                        std::variant<const Yieldable*, YieldThroughAcquisitions> yieldable,
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
    PlanYieldPolicyClassicTrialPeriod(
        OperationContext* opCtx,
        PlanStage* root,
        PlanYieldPolicy::YieldPolicy policy,
        std::variant<const Yieldable*, YieldThroughAcquisitions> yieldable,
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
std::unique_ptr<PlanYieldPolicy> makeClassicYieldPolicy(
    OperationContext* opCtx,
    NamespaceString nss,
    PlanType* plan,
    PlanYieldPolicy::YieldPolicy policy,
    VariantCollectionPtrOrAcquisition collection) {
    const std::variant<const Yieldable*, PlanYieldPolicy::YieldThroughAcquisitions> yieldable =
        visit(OverloadedVisitor{[](const CollectionPtr* coll) {
                                    return std::variant<const Yieldable*,
                                                        PlanYieldPolicy::YieldThroughAcquisitions>(
                                        *coll ? coll : nullptr);
                                },
                                [](const CollectionAcquisition& coll) {
                                    return std::variant<const Yieldable*,
                                                        PlanYieldPolicy::YieldThroughAcquisitions>(
                                        PlanYieldPolicy::YieldThroughAcquisitions{});
                                }},
              collection.get());

    using PolicyType = std::conditional_t<std::is_same_v<PlanType, PlanStage>,
                                          PlanYieldPolicyClassicTrialPeriod,
                                          PlanYieldPolicyImpl>;

    switch (policy) {
        case PlanYieldPolicy::YieldPolicy::YIELD_AUTO:
        case PlanYieldPolicy::YieldPolicy::YIELD_MANUAL:
        case PlanYieldPolicy::YieldPolicy::WRITE_CONFLICT_RETRY_ONLY:
        case PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY: {
            return std::make_unique<PolicyType>(
                opCtx,
                plan,
                policy,
                yieldable,
                std::make_unique<YieldPolicyCallbacksImpl>(std::move(nss)));
        }
        case PlanYieldPolicy::YieldPolicy::ALWAYS_TIME_OUT: {
            return std::make_unique<AlwaysTimeOutYieldPolicy>(
                opCtx, &opCtx->fastClockSource(), yieldable);
        }
        case PlanYieldPolicy::YieldPolicy::ALWAYS_MARK_KILLED: {
            return std::make_unique<AlwaysPlanKilledYieldPolicy>(
                opCtx, &opCtx->fastClockSource(), yieldable);
        }
    }
    MONGO_UNREACHABLE;
}

}  // namespace mongo
