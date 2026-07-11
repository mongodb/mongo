// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_yield_policy_sbe.h"

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/query/yield_policy_callbacks_impl.h"

namespace mongo {
std::unique_ptr<PlanYieldPolicySBE> PlanYieldPolicySBE::make(
    OperationContext* opCtx,
    PlanYieldPolicy::YieldPolicy policy,
    const MultipleCollectionAccessor& collections,
    NamespaceString nss) {
    return make(opCtx,
                policy,
                &opCtx->fastClockSource(),
                internalQueryExecYieldIterations.load(),
                Milliseconds{internalQueryExecYieldPeriodMS.load()},
                std::make_unique<YieldPolicyCallbacksImpl>(std::move(nss)));
}

std::unique_ptr<PlanYieldPolicySBE> PlanYieldPolicySBE::make(
    OperationContext* opCtx,
    YieldPolicy policy,
    ClockSource* clockSource,
    int yieldFrequency,
    Milliseconds yieldPeriod,
    std::unique_ptr<YieldPolicyCallbacks> callbacks) {
    return std::unique_ptr<PlanYieldPolicySBE>(new PlanYieldPolicySBE(
        opCtx, policy, clockSource, yieldFrequency, yieldPeriod, std::move(callbacks)));
}

PlanYieldPolicySBE::PlanYieldPolicySBE(OperationContext* opCtx,
                                       YieldPolicy policy,
                                       ClockSource* clockSource,
                                       int yieldFrequency,
                                       Milliseconds yieldPeriod,
                                       std::unique_ptr<YieldPolicyCallbacks> callbacks)
    : PlanYieldPolicy(
          opCtx, policy, clockSource, yieldFrequency, yieldPeriod, std::move(callbacks)) {
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
    uassertIfPathArraynessInvalidated();
}

void PlanYieldPolicySBE::MultipleCollectionPathArraynessChecker::
    uassertIfInvalidatedAndSyncEpoch() {
    collections.forEach([&](const CollectionPtr& coll) {
        if (!coll) {
            return;
        }
        auto it = perNss.find(coll->ns());
        if (it == perNss.end() || it->second.nonArrayPaths.empty()) {
            return;
        }
        auto current = getPathArrayness(coll);
        tassert(12567300, "Expected path arrayness to be set on CollectionQueryInfo", current);
        it->second.uassertIfInvalidatedAndSyncEpoch(*current, coll->ns());
    });
}

void PlanYieldPolicySBE::setMultipleCollectionPathArraynessChecker(
    MultipleCollectionPathArraynessChecker checker) {
    _multipleCollectionsPathArraynessChecker.emplace(std::move(checker));
}

void PlanYieldPolicySBE::uassertIfPathArraynessInvalidated() {
    if (_multipleCollectionsPathArraynessChecker) {
        _multipleCollectionsPathArraynessChecker->uassertIfInvalidatedAndSyncEpoch();
    }
}

}  // namespace mongo
