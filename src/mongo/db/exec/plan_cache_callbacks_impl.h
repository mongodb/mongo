// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/multi_plan_rate_limiter.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/plan_cache/plan_cache_callbacks.h"
#include "mongo/db/query/plan_cache/plan_cache_log_utils.h"
#include "mongo/db/query/plan_cache/sbe_plan_cache.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/modules.h"

namespace mongo {
template <class CachedPlanType, class DebugInfo>
class PlanCacheEntryBase;
struct SolutionCacheData;

/**
 * Callbacks implementation for the plan cache.
 */
template <typename KeyType, typename CachedPlanType, typename DebugInfoType>
class PlanCacheCallbacksImpl : public PlanCacheCallbacks<KeyType, CachedPlanType, DebugInfoType> {
public:
    PlanCacheCallbacksImpl(const CanonicalQuery& cq,
                           std::function<DebugInfoType()> buildDebugInfo,
                           std::function<std::string(const CachedPlanType&)> printCachedPlan,
                           const CollectionPtr& collection)
        : _cq{cq},
          _buildDebugInfoCallback(buildDebugInfo),
          _printCachedPlanCallback(printCachedPlan),
          _collection(collection) {
        tassert(6407401, "_buildDebugInfoCallBack should be callable", _buildDebugInfoCallback);
        tassert(8983105, "_printCachedPlanCallback should be callable", _printCachedPlanCallback);
    }

    void onCreateInactiveCacheEntry(
        const KeyType& key,
        const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry,
        PlanCacheDecisionMetrics newPlanCacheDecisionMetrics) const final {
        auto&& [planCacheShapeHash, planCacheKey] = hashes(key, oldEntry);
        log_detail::logCreateInactiveCacheEntry(_cq.toStringShort(),
                                                std::move(planCacheShapeHash),
                                                std::move(planCacheKey),
                                                newPlanCacheDecisionMetrics);
    }

    void onReplaceActiveCacheEntry(
        const KeyType& key,
        const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry,
        PlanCacheDecisionMetrics newPlanCacheDecisionMetrics) const final {
        tassert(1003130, "Expected oldEntry to not be null", oldEntry);
        tassert(1003131,
                "oldEntry is expected to have non zero works",
                oldEntry->planCacheDecisionMetrics);
        auto&& [planCacheShapeHash, planCacheKey] = hashes(key, oldEntry);
        log_detail::logReplaceActiveCacheEntry(_cq.toStringShort(),
                                               std::move(planCacheShapeHash),
                                               std::move(planCacheKey),
                                               *oldEntry->planCacheDecisionMetrics,
                                               newPlanCacheDecisionMetrics);
    }

    void onNoopActiveCacheEntry(const KeyType& key,
                                const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry,
                                PlanCacheDecisionMetrics newPlanCacheDecisionMetrics) const final {
        tassert(1003132, "Expected oldEntry to not be null", oldEntry);
        tassert(1003133,
                "oldEntry is expected to have non zero works",
                oldEntry->planCacheDecisionMetrics);
        auto&& [planCacheShapeHash, planCacheKey] = hashes(key, oldEntry);
        log_detail::logNoop(_cq.toStringShort(),
                            std::move(planCacheShapeHash),
                            std::move(planCacheKey),
                            *oldEntry->planCacheDecisionMetrics,
                            newPlanCacheDecisionMetrics);
    }

    void onIncreasingWorkValue(const KeyType& key,
                               const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry,
                               PlanCacheDecisionMetrics newPlanCacheDecisionMetrics) const final {
        tassert(1003134, "Expected oldEntry to not be null", oldEntry);
        tassert(1003135,
                "oldEntry is expected to have non zero works",
                oldEntry->planCacheDecisionMetrics);
        auto&& [planCacheShapeHash, planCacheKey] = hashes(key, oldEntry);
        log_detail::logIncreasingWorkValue(_cq.toStringShort(),
                                           std::move(planCacheShapeHash),
                                           std::move(planCacheKey),
                                           *oldEntry->planCacheDecisionMetrics,
                                           newPlanCacheDecisionMetrics);
    }

    void onPromoteCacheEntry(const KeyType& key,
                             const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry,
                             const CachedPlanType& newPlan,
                             PlanCacheDecisionMetrics newPlanCacheDecisionMetrics) const final {
        tassert(1003136, "Expected oldEntry to not be null", oldEntry);
        tassert(1003137,
                "oldEntry is expected to have non zero works",
                oldEntry->planCacheDecisionMetrics);
        auto&& [planCacheShapeHash, planCacheKey] = hashes(key, oldEntry);

        auto* serviceContext = getCurrentServiceContext();
        if (serviceContext) {
            /*
             * Since the plan cache entry is now active, multiplanning will be skipped the next time
             * the query shape is encountered  and the cached plan will be immediately trialed. For
             * this reason, the query shape can be removed from the data structure maintaining the
             * rate limited query shapes.
             */
            MultiPlanRateLimiter::get(serviceContext)
                .removeQueryShapeFromMultiPlanRateLimiter(_collection, key.toString());
        }

        if (oldEntry->cachedPlan->solutionHash != newPlan.solutionHash) {
            if constexpr (std::is_same_v<CachedPlanType, sbe::CachedSbePlan>) {
                planCacheCounters.incrementSbeInactiveCachedPlansReplacedCounter();
            } else {
                planCacheCounters.incrementClassicInactiveCachedPlansReplacedCounter();
            }
        }

        log_detail::logPromoteCacheEntry(_cq.toStringShort(),
                                         std::move(planCacheShapeHash),
                                         std::move(planCacheKey),
                                         *oldEntry->planCacheDecisionMetrics,
                                         newPlanCacheDecisionMetrics);
    }

    void onUnexpectedPinnedCacheEntry(
        const KeyType& key,
        const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry,
        const CachedPlanType& newPlan,
        PlanCacheDecisionMetrics newPlanCacheDecisionMetrics) const final {
        tassert(8983101, "Expected oldEntry to not be null", oldEntry);
        tassert(8983102, "Expected oldEntry to be pinned", !oldEntry->planCacheDecisionMetrics);
        auto&& [planCacheShapeHash, planCacheKey] = hashes(key, oldEntry);
        auto newEntryDebugInfo = buildDebugInfo();
        log_detail::logUnexpectedPinnedCacheEntry(_cq.toStringShort(),
                                                  std::move(planCacheShapeHash),
                                                  std::move(planCacheKey),
                                                  oldEntry->debugString(),
                                                  newEntryDebugInfo.debugString(),
                                                  printCachedPlan(*oldEntry->cachedPlan.get()),
                                                  printCachedPlan(newPlan),
                                                  newPlanCacheDecisionMetrics);
    }

    DebugInfoType buildDebugInfo() const final {
        return _buildDebugInfoCallback();
    }

    uint32_t getPlanCacheCommandKeyHash() const final {
        return canonical_query_encoder::computeHash(
            canonical_query_encoder::encodeForPlanCacheCommand(_cq));
    }

private:
    std::string printCachedPlan(const CachedPlanType& plan) const {
        return _printCachedPlanCallback(plan);
    }

    auto hashes(const KeyType& key,
                const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry) const {
        // Avoid recomputing the hashes if we've got an old entry to grab them from.
        return oldEntry ? std::make_pair(zeroPaddedHex(oldEntry->planCacheShapeHash),
                                         zeroPaddedHex(oldEntry->planCacheKey))
                        : std::make_pair(zeroPaddedHex(key.planCacheShapeHash()),
                                         zeroPaddedHex(key.planCacheKeyHash()));
    }

    const CanonicalQuery& _cq;
    std::function<DebugInfoType()> _buildDebugInfoCallback;
    std::function<std::string(const CachedPlanType&)> _printCachedPlanCallback;
    const CollectionPtr& _collection;
};
}  // namespace mongo
