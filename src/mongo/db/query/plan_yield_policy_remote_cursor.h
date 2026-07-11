// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

/**
 * A PlanYieldPolicy for remote cursors to yield and release storage resources during network call.
 */
class PlanYieldPolicyRemoteCursor final : public PlanYieldPolicy {
public:
    static std::unique_ptr<PlanYieldPolicyRemoteCursor> make(
        OperationContext* opCtx,
        PlanYieldPolicy::YieldPolicy policy,
        const MultipleCollectionAccessor& collections,
        NamespaceString nss,
        PlanExecutor* exec = nullptr);

    void registerPlanExecutor(PlanExecutor* exec) {
        _exec = exec;
    }

private:
    PlanYieldPolicyRemoteCursor(OperationContext* opCtx,
                                PlanYieldPolicy::YieldPolicy policy,
                                std::unique_ptr<YieldPolicyCallbacks> callbacks,
                                PlanExecutor* exec = nullptr);

    void saveState(OperationContext* opCtx) override;

    void restoreState(OperationContext* opCtx,
                      const Yieldable* yieldable,
                      RestoreContext::RestoreType restoreType) override;

    // The plan executor which this yield policy is responsible for yielding.
    PlanExecutor* _exec;
};

}  // namespace mongo
