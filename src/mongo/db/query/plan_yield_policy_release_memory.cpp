/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/plan_yield_policy_release_memory.h"

#include "mongo/db/query/query_knobs_gen.h"
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
