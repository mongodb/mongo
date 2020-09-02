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

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/query/plan_yield_policy.h"

namespace mongo {

class PlanYieldPolicySBE final : public PlanYieldPolicy {
public:
    PlanYieldPolicySBE(YieldPolicy policy,
                       ClockSource* clockSource,
                       int yieldFrequency,
                       Milliseconds yieldPeriod,
                       std::function<void(OperationContext*)> duringAllYieldsFn)
        : PlanYieldPolicy(policy, clockSource, yieldFrequency, yieldPeriod),
          _duringAllYieldsFn(std::move(duringAllYieldsFn)) {
        uassert(4822879,
                "WRITE_CONFLICT_RETRY_ONLY yield policy is not supported in SBE",
                policy != YieldPolicy::WRITE_CONFLICT_RETRY_ONLY);
    }

    /**
     * Registers the tree rooted at 'plan' to yield, in addition to all other plans that have been
     * previously registered with this yield policy.
     */
    void registerPlan(sbe::PlanStage* plan) {
        _yieldingPlans.push_back(plan);
    }

    /**
     * Clears the list of plans currently registered to yield.
     */
    void clearRegisteredPlans() {
        _yieldingPlans.clear();
    }

private:
    Status yield(OperationContext* opCtx, std::function<void()> whileYieldingFn = nullptr) override;

    // A function provided on construction which gets called every time a yield is triggered. This
    // is in contrast to the 'whileYieldFn'parameter to the 'yield()' method, which can be different
    // on each yield. Both functions will get called as part of an SBE plan yielding.
    std::function<void(OperationContext*)> _duringAllYieldsFn;

    // The list of plans registered to yield when the configured policy triggers a yield.
    std::vector<sbe::PlanStage*> _yieldingPlans;
};

}  // namespace mongo
