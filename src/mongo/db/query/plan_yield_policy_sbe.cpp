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

#include "mongo/db/query/plan_yield_policy_sbe.h"

#include "mongo/db/query/yield_policy_callbacks_impl.h"
#include "mongo/db/storage/storage_parameters_gen.h"

namespace mongo {
std::unique_ptr<PlanYieldPolicySBE> PlanYieldPolicySBE::make(
    OperationContext* opCtx,
    PlanYieldPolicy::YieldPolicy policy,
    const MultipleCollectionAccessor& collections,
    NamespaceString nss) {

    std::variant<const Yieldable*, PlanYieldPolicy::YieldThroughAcquisitions> yieldable;
    if (collections.isAcquisition()) {
        yieldable = PlanYieldPolicy::YieldThroughAcquisitions{};
    } else {
        yieldable = &collections.getMainCollection();
    }

    return make(opCtx,
                policy,
                &opCtx->fastClockSource(),
                internalQueryExecYieldIterations.load(),
                Milliseconds{internalQueryExecYieldPeriodMS.load()},
                yieldable,
                std::make_unique<YieldPolicyCallbacksImpl>(std::move(nss)));
}

std::unique_ptr<PlanYieldPolicySBE> PlanYieldPolicySBE::make(
    OperationContext* opCtx,
    YieldPolicy policy,
    ClockSource* clockSource,
    int yieldFrequency,
    Milliseconds yieldPeriod,
    std::variant<const Yieldable*, YieldThroughAcquisitions> yieldable,
    std::unique_ptr<YieldPolicyCallbacks> callbacks) {
    return std::unique_ptr<PlanYieldPolicySBE>(new PlanYieldPolicySBE(
        opCtx, policy, clockSource, yieldFrequency, yieldPeriod, yieldable, std::move(callbacks)));
}

PlanYieldPolicySBE::PlanYieldPolicySBE(
    OperationContext* opCtx,
    YieldPolicy policy,
    ClockSource* clockSource,
    int yieldFrequency,
    Milliseconds yieldPeriod,
    std::variant<const Yieldable*, YieldThroughAcquisitions> yieldable,
    std::unique_ptr<YieldPolicyCallbacks> callbacks)
    : PlanYieldPolicy(opCtx,
                      policy,
                      clockSource,
                      yieldFrequency,
                      yieldPeriod,
                      yieldable,
                      std::move(callbacks)) {
    uassert(4822879,
            "WRITE_CONFLICT_RETRY_ONLY yield policy is not supported in SBE",
            policy != YieldPolicy::WRITE_CONFLICT_RETRY_ONLY);

    // TODO SERVER-103267: Remove gYieldingSupportForSBE.
}

void PlanYieldPolicySBE::saveState(OperationContext* opCtx) {
    for (auto&& root : _yieldingPlans) {
        root->saveState();
    }
}

void PlanYieldPolicySBE::restoreState(OperationContext* opCtx,
                                      const Yieldable*,
                                      RestoreContext::RestoreType restoreType) {
    for (auto&& root : _yieldingPlans) {
        root->restoreState();
    }
}
}  // namespace mongo
