// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/yieldable.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/optional.hpp>

namespace mongo {

/**
 * PlanYieldPolicy specifically for releaseMemory command. It is required because this command
 * can operate on a PlanExecutor in a "saved" state without calling restoreState().
 */
class PlanYieldPolicyReleaseMemory final : public PlanYieldPolicy {
public:
    PlanYieldPolicyReleaseMemory(OperationContext* opCtx,
                                 PlanYieldPolicy::YieldPolicy policy,
                                 std::unique_ptr<YieldPolicyCallbacks> callbacks);

    static std::unique_ptr<PlanYieldPolicyReleaseMemory> make(OperationContext* opCtx,
                                                              PlanYieldPolicy::YieldPolicy policy,
                                                              NamespaceString nss);

private:
    void saveState(OperationContext* opCtx) override;

    void restoreState(OperationContext* opCtx,
                      const Yieldable* yieldable,
                      RestoreContext::RestoreType restoreType) override;
};

}  // namespace mongo
