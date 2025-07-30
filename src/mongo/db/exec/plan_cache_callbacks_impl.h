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

#pragma once

#include "mongo/db/exec/classic/multi_plan_rate_limiter.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/plan_cache/plan_cache_callbacks.h"
#include "mongo/db/query/plan_cache/plan_cache_log_utils.h"
#include "mongo/db/query/plan_cache/sbe_plan_cache.h"
#include "mongo/db/stats/counters.h"

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
        size_t newWorks) const final {
        auto&& [planCacheShapeHash, planCacheKey] = hashes(key, oldEntry);
        log_detail::logCreateInactiveCacheEntry(
            _cq.toStringShort(), std::move(planCacheShapeHash), std::move(planCacheKey), newWorks);
    }

    void onReplaceActiveCacheEntry(
        const KeyType& key,
        const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry,
        size_t newWorks) const final {
        tassert(1003130, "Expected oldEntry to not be null", oldEntry);
        tassert(1003131, "oldEntry is expected to have non zero works", oldEntry->readsOrWorks);
        auto&& [planCacheShapeHash, planCacheKey] = hashes(key, oldEntry);
        log_detail::logReplaceActiveCacheEntry(_cq.toStringShort(),
                                               std::move(planCacheShapeHash),
                                               std::move(planCacheKey),
                                               oldEntry->readsOrWorks->rawValue(),
                                               newWorks);
    }

    void onNoopActiveCacheEntry(const KeyType& key,
                                const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry,
                                size_t newWorks) const final {
        tassert(1003132, "Expected oldEntry to not be null", oldEntry);
        tassert(1003133, "oldEntry is expected to have non zero works", oldEntry->readsOrWorks);
        auto&& [planCacheShapeHash, planCacheKey] = hashes(key, oldEntry);
        log_detail::logNoop(_cq.toStringShort(),
                            std::move(planCacheShapeHash),
                            std::move(planCacheKey),
                            oldEntry->readsOrWorks->rawValue(),
                            newWorks);
    }

    void onIncreasingWorkValue(const KeyType& key,
                               const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry,
                               size_t newWorks) const final {
        tassert(1003134, "Expected oldEntry to not be null", oldEntry);
        tassert(1003135, "oldEntry is expected to have non zero works", oldEntry->readsOrWorks);
        auto&& [planCacheShapeHash, planCacheKey] = hashes(key, oldEntry);
        log_detail::logIncreasingWorkValue(_cq.toStringShort(),
                                           std::move(planCacheShapeHash),
                                           std::move(planCacheKey),
                                           oldEntry->readsOrWorks->rawValue(),
                                           newWorks);
    }

    void onPromoteCacheEntry(const KeyType& key,
                             const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry,
                             const CachedPlanType& newPlan,
                             size_t newWorks) const final {
        tassert(1003136, "Expected oldEntry to not be null", oldEntry);
        tassert(1003137, "oldEntry is expected to have non zero works", oldEntry->readsOrWorks);
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
                                         oldEntry->readsOrWorks->rawValue(),
                                         newWorks);
    }

    void onUnexpectedPinnedCacheEntry(
        const KeyType& key,
        const PlanCacheEntryBase<CachedPlanType, DebugInfoType>* oldEntry,
        const CachedPlanType& newPlan,
        size_t newWorks) const final {
        tassert(8983101, "Expected oldEntry to not be null", oldEntry);
        tassert(8983102, "Expected oldEntry to be pinned", !oldEntry->readsOrWorks);
        auto&& [planCacheShapeHash, planCacheKey] = hashes(key, oldEntry);
        auto newEntryDebugInfo = buildDebugInfo();
        log_detail::logUnexpectedPinnedCacheEntry(_cq.toStringShort(),
                                                  std::move(planCacheShapeHash),
                                                  std::move(planCacheKey),
                                                  oldEntry->debugString(),
                                                  newEntryDebugInfo.debugString(),
                                                  printCachedPlan(*oldEntry->cachedPlan.get()),
                                                  printCachedPlan(newPlan),
                                                  newWorks);
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
