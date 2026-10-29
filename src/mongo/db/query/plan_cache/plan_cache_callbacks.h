// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/plan_cache/plan_cache_decision_metrics.h"
#include "mongo/util/modules.h"

namespace mongo {

template <class CachedPlanType, class DebugInfo>
class PlanCacheEntryBase;
struct SolutionCacheData;

/**
 * Encapsulates callback functions used to perform a custom action when the plan cache state
 * changes.
 */
template <typename KeyType, typename CachedPlanType, typename DebugInfoType>
class PlanCacheCallbacks {
public:
    virtual ~PlanCacheCallbacks() = default;

    virtual void onCreateInactiveCacheEntry(
        const KeyType& key,
        const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry,
        PlanCacheDecisionMetrics newPlanCacheDecisionMetrics) const = 0;
    virtual void onReplaceActiveCacheEntry(
        const KeyType& key,
        const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry,
        PlanCacheDecisionMetrics newPlanCacheDecisionMetrics) const = 0;
    virtual void onNoopActiveCacheEntry(
        const KeyType& key,
        const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry,
        PlanCacheDecisionMetrics newPlanCacheDecisionMetrics) const = 0;
    virtual void onIncreasingWorkValue(
        const KeyType& key,
        const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry,
        PlanCacheDecisionMetrics newPlanCacheDecisionMetrics) const = 0;
    virtual void onPromoteCacheEntry(
        const KeyType& key,
        const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry,
        const CachedPlanType& newPlan,
        PlanCacheDecisionMetrics newPlanCacheDecisionMetrics) const = 0;
    virtual void onUnexpectedPinnedCacheEntry(
        const KeyType& key,
        const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry,
        const CachedPlanType& newPlan,
        PlanCacheDecisionMetrics newPlanCacheDecisionMetrics) const = 0;
    virtual DebugInfoType buildDebugInfo() const = 0;
    virtual uint32_t getPlanCacheCommandKeyHash() const = 0;
};

}  // namespace mongo
