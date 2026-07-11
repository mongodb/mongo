// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/metadata/path_arrayness.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <functional>
#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {
class PlanStage;
}  // namespace mongo::sbe

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

    struct MultipleCollectionPathArraynessChecker {
        MultipleCollectionAccessor collections;
        stdx::unordered_map<NamespaceString, PathArraynessChecker> perNss;
        // Returns current PathArrayness for a collection. Caller supplies this so
        // PlanYieldPolicySBE need not depend on CollectionQueryInfo directly.
        std::function<std::shared_ptr<const PathArrayness>(const CollectionPtr&)> getPathArrayness;

        void uassertIfInvalidatedAndSyncEpoch();
    };

    /**
     * Registers a path arrayness check to run after each yield/restore cycle.
     */
    void setMultipleCollectionPathArraynessChecker(MultipleCollectionPathArraynessChecker checker);

private:
    void uassertIfPathArraynessInvalidated();
    PlanYieldPolicySBE(OperationContext* opCtx,
                       YieldPolicy policy,
                       ClockSource* clockSource,
                       int yieldFrequency,
                       Milliseconds yieldPeriod,
                       std::unique_ptr<YieldPolicyCallbacks> callbacks);

    void saveState(OperationContext* opCtx) override;

    void restoreState(OperationContext* opCtx,
                      const Yieldable* yieldable,
                      RestoreContext::RestoreType restoreType) override;

    // The list of plans registered to yield when the configured policy triggers a yield.
    std::vector<sbe::PlanStage*> _yieldingPlans;

    boost::optional<MultipleCollectionPathArraynessChecker>
        _multipleCollectionsPathArraynessChecker;
};

}  // namespace mongo
