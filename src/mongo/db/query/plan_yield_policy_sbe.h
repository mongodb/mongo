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
#include "mongo/db/operation_context.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"

#include <memory>
#include <variant>
#include <vector>

namespace mongo {

class PlanYieldPolicySBE final : public PlanYieldPolicy {
public:
    static std::unique_ptr<PlanYieldPolicySBE> make(OperationContext* opCtx,
                                                    PlanYieldPolicy::YieldPolicy policy,
                                                    const MultipleCollectionAccessor& collections,
                                                    NamespaceString nss);

    static std::unique_ptr<PlanYieldPolicySBE> make(
        OperationContext* opCtx,
        YieldPolicy policy,
        ClockSource* clockSource,
        int yieldFrequency,
        Milliseconds yieldPeriod,
        std::variant<const Yieldable*, YieldThroughAcquisitions> yieldable,
        std::unique_ptr<YieldPolicyCallbacks> callbacks = nullptr);

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
    PlanYieldPolicySBE(OperationContext* opCtx,
                       YieldPolicy policy,
                       ClockSource* clockSource,
                       int yieldFrequency,
                       Milliseconds yieldPeriod,
                       std::variant<const Yieldable*, YieldThroughAcquisitions> yieldable,
                       std::unique_ptr<YieldPolicyCallbacks> callbacks);

    void saveState(OperationContext* opCtx) override;

    void restoreState(OperationContext* opCtx,
                      const Yieldable* yieldable,
                      RestoreContext::RestoreType restoreType) override;

    // The list of plans registered to yield when the configured policy triggers a yield.
    std::vector<sbe::PlanStage*> _yieldingPlans;
};

}  // namespace mongo
