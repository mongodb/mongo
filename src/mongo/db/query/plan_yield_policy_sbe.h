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
#include "mongo/db/storage/storage_parameters_gen.h"

namespace mongo {

class PlanYieldPolicySBE final : public PlanYieldPolicy {
public:
    PlanYieldPolicySBE(YieldPolicy policy,
                       ClockSource* clockSource,
                       int yieldFrequency,
                       Milliseconds yieldPeriod,
                       const Yieldable* yieldable,
                       std::unique_ptr<YieldPolicyCallbacks> callbacks)
        : PlanYieldPolicy(
              policy, clockSource, yieldFrequency, yieldPeriod, yieldable, std::move(callbacks)),
          _useExperimentalCommitTxnBehavior(gYieldingSupportForSBE) {
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
    void saveState(OperationContext* opCtx) override;

    void restoreState(OperationContext* opCtx, const Yieldable* yieldable) override;

    // TODO SERVER-59620: Remove this.
    bool useExperimentalCommitTxnBehavior() const override {
        return _useExperimentalCommitTxnBehavior;
    }

    // The list of plans registered to yield when the configured policy triggers a yield.
    std::vector<sbe::PlanStage*> _yieldingPlans;

    // Whether the experimental behavior which commits transactions across yields instead of
    // aborting them, should be used.
    bool _useExperimentalCommitTxnBehavior;
};

}  // namespace mongo
