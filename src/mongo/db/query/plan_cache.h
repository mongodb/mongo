/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/query/lru_key_value.h"
#include "mongo/db/query/plan_cache_callbacks.h"
#include "mongo/db/query/plan_cache_debug_info.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/container_size_helper.h"

namespace mongo {
class QuerySolution;
struct QuerySolutionNode;

template <class CachedPlanType>
class PlanCacheEntryBase;

/**
 * Information returned from a get(...) query.
 */
template <class CachedPlanType>
class CachedPlanHolder {
private:
    CachedPlanHolder(const CachedPlanHolder&) = delete;
    CachedPlanHolder& operator=(const CachedPlanHolder&) = delete;

public:
    CachedPlanHolder(const PlanCacheEntryBase<CachedPlanType>& entry)
        : cachedPlan(entry.cachedPlan->clone()), decisionWorks(entry.works) {}


    // A cached plan that can be used to reconstitute the complete execution plan from cache.
    std::unique_ptr<CachedPlanType> cachedPlan;

    // The number of work cycles taken to decide on a winning plan when the plan was first
    // cached.
    const size_t decisionWorks;
};

/**
 * Used by the cache to track entries and their performance over time.
 * Also used by the plan cache commands to display plan cache state.
 */
template <class CachedPlanType>
class PlanCacheEntryBase {
public:
    template <typename KeyType>
    static std::unique_ptr<PlanCacheEntryBase<CachedPlanType>> create(
        const std::vector<QuerySolution*>& solutions,
        std::unique_ptr<const plan_ranker::PlanRankingDecision> decision,
        std::unique_ptr<CachedPlanType> cachedPlan,
        uint32_t queryHash,
        uint32_t planCacheKey,
        Date_t timeOfCreation,
        bool isActive,
        size_t works,
        const PlanCacheCallbacks<KeyType, CachedPlanType>* callbacks) {
        invariant(decision);

        // If the cumulative size of the plan caches is estimated to remain within a predefined
        // threshold, then then include additional debug info which is not strictly necessary for
        // the plan cache to be functional. Once the cumulative plan cache size exceeds this
        // threshold, omit this debug info as a heuristic to prevent plan cache memory consumption
        // from growing too large.
        const bool includeDebugInfo = planCacheTotalSizeEstimateBytes.get() <
            internalQueryCacheMaxSizeBytesBeforeStripDebugInfo.load();

        boost::optional<plan_cache_debug_info::DebugInfo> debugInfo;
        if (includeDebugInfo && callbacks) {
            debugInfo.emplace(callbacks->buildDebugInfo(std::move(decision)));
        }

        return std::unique_ptr<PlanCacheEntryBase<CachedPlanType>>(
            new PlanCacheEntryBase<CachedPlanType>(std::move(cachedPlan),
                                                   timeOfCreation,
                                                   queryHash,
                                                   planCacheKey,
                                                   isActive,
                                                   works,
                                                   std::move(debugInfo)));
    }

    ~PlanCacheEntryBase() {
        planCacheTotalSizeEstimateBytes.decrement(estimatedEntrySizeBytes);
    }

    /**
     * Make a deep copy.
     */
    std::unique_ptr<PlanCacheEntryBase<CachedPlanType>> clone() const {
        boost::optional<plan_cache_debug_info::DebugInfo> debugInfoCopy;
        if (debugInfo) {
            debugInfoCopy.emplace(*debugInfo);
        }

        return std::unique_ptr<PlanCacheEntryBase<CachedPlanType>>(
            new PlanCacheEntryBase<CachedPlanType>(cachedPlan->clone(),
                                                   timeOfCreation,
                                                   queryHash,
                                                   planCacheKey,
                                                   isActive,
                                                   works,
                                                   std::move(debugInfoCopy)));
    }

    std::string debugString() const {
        StringBuilder builder;
        builder << "(";
        builder << "queryHash: " << queryHash;
        builder << "; planCacheKey: " << planCacheKey;
        if (debugInfo) {
            builder << "; ";
            builder << debugInfo->createdFromQuery.debugString();
        }
        builder << "; timeOfCreation: " << timeOfCreation.toString() << ")";
        return builder.str();
    }

    // A cached plan that can be used to reconstitute the complete execution plan from cache.
    const std::unique_ptr<CachedPlanType> cachedPlan;

    const Date_t timeOfCreation;

    // Hash of the cache key. Intended as an identifier for the query shape in logs and other
    // diagnostic output.
    const uint32_t queryHash;

    // Hash of the "stable" cache key, which is the same regardless of what indexes are around.
    const uint32_t planCacheKey;

    // Whether or not the cache entry is active. Inactive cache entries should not be used for
    // planning.
    bool isActive = false;

    // The number of "works" required for a plan to run on this shape before it becomes
    // active. This value is also used to determine the number of works necessary in order to
    // trigger a replan. Running a query of the same shape while this cache entry is inactive may
    // cause this value to be increased.
    size_t works = 0;

    // Optional debug info containing detailed statistics. Includes a description of the query which
    // resulted in this plan cache's creation as well as runtime stats from the multi-planner trial
    // period that resulted in this cache entry.
    //
    // Once the estimated cumulative size of the mongod's plan caches exceeds a threshold, this
    // debug info is omitted from new plan cache entries.
    const boost::optional<plan_cache_debug_info::DebugInfo> debugInfo;

    // An estimate of the size in bytes of this plan cache entry. This is the "deep size",
    // calculated by recursively incorporating the size of owned objects, the objects that they in
    // turn own, and so on.
    const uint64_t estimatedEntrySizeBytes;

    /**
     * Tracks the approximate cumulative size of the plan cache entries across all the collections.
     */
    inline static Counter64 planCacheTotalSizeEstimateBytes;

private:
    /**
     * All arguments constructor.
     */
    PlanCacheEntryBase(std::unique_ptr<CachedPlanType> cachedPlan,
                       Date_t timeOfCreation,
                       uint32_t queryHash,
                       uint32_t planCacheKey,
                       bool isActive,
                       size_t works,
                       boost::optional<plan_cache_debug_info::DebugInfo> debugInfo)
        : cachedPlan(std::move(cachedPlan)),
          timeOfCreation(timeOfCreation),
          queryHash(queryHash),
          planCacheKey(planCacheKey),
          isActive(isActive),
          works(works),
          debugInfo(std::move(debugInfo)),
          estimatedEntrySizeBytes(_estimateObjectSizeInBytes()) {
        invariant(this->cachedPlan);
        // Account for the object in the global metric for estimating the server's total plan cache
        // memory consumption.
        planCacheTotalSizeEstimateBytes.increment(estimatedEntrySizeBytes);
    }

    // Ensure that PlanCacheEntryBase is non-copyable.
    PlanCacheEntryBase(const PlanCacheEntryBase&) = delete;
    PlanCacheEntryBase& operator=(const PlanCacheEntryBase&) = delete;

    uint64_t _estimateObjectSizeInBytes() const {
        uint64_t size = sizeof(PlanCacheEntryBase);
        size += cachedPlan->estimateObjectSizeInBytes();

        if (debugInfo) {
            size += debugInfo->estimateObjectSizeInBytes();
        }

        return size;
    }
};

/**
 * A data structure for caching execution plans, to avoid repeatedly performing query optimization
 * and plan compilation on each invocation of a query. The cache is logically a mapping from
 * 'KeyType' to 'CachedPlanType'. The cache key is derived from the query, and can be used to
 * determine whether a cached plan is available. The cache has an LRU replacement policy, so it only
 * keeps the most recently used plans.
 */
template <class KeyType,
          class CachedPlanType,
          class BudgetEstimator,
          class KeyHasher = std::hash<KeyType>>
class PlanCacheBase {
private:
    PlanCacheBase(const PlanCacheBase&) = delete;
    PlanCacheBase& operator=(const PlanCacheBase&) = delete;

public:
    using Entry = PlanCacheEntryBase<CachedPlanType>;

    // We have three states for a cache entry to be in. Rather than just 'present' or 'not
    // present', we use a notion of 'inactive entries' as a way of remembering how performant our
    // original solution to the query was. This information is useful to prevent much slower
    // queries from putting their plans in the cache immediately, which could cause faster queries
    // to run with a sub-optimal plan. Since cache entries must go through the "vetting" process of
    // being inactive, we protect ourselves from the possibility of simply adding a cache entry
    // with a very high works value which will never be evicted.
    enum CacheEntryState {
        // There is no cache entry for the given query shape.
        kNotPresent,

        // There is a cache entry for the given query shape, but it is inactive, meaning that it
        // should not be used when planning.
        kPresentInactive,

        // There is a cache entry for the given query shape, and it is active.
        kPresentActive,
    };

    /**
     * Encapsulates the value returned from a call to get().
     */
    struct GetResult {
        CacheEntryState state;
        std::unique_ptr<CachedPlanHolder<CachedPlanType>> cachedPlanHolder;
    };

    PlanCacheBase(size_t size) : _cache{size} {}

    ~PlanCacheBase() = default;

    /**
     * Record solutions for query. Best plan is first element in list.
     * Each query in the cache will have more than 1 plan because we only
     * add queries which are considered by the multi plan runner (which happens
     * only when the query planner generates multiple candidate plans). Callers are responsible
     * for passing the current time so that the time the plan cache entry was created is stored
     * in the plan cache.
     *
     * 'worksGrowthCoefficient' specifies what multiplier to use when growing the 'works' value of
     * an inactive cache entry.  If boost::none is provided, the function will use
     * 'internalQueryCacheWorksGrowthCoefficient'.
     *
     * A 'callbacks' argument can be provided to perform some custom actions when the state of the
     * plan cache or a plan cache entry has been changed.
     *
     * If the mapping was set successfully, returns Status::OK(), even if it evicted another entry.
     */
    Status set(const KeyType& key,
               std::unique_ptr<CachedPlanType> cachedPlan,
               const std::vector<QuerySolution*>& solns,
               std::unique_ptr<plan_ranker::PlanRankingDecision> why,
               Date_t now,
               boost::optional<double> worksGrowthCoefficient = boost::none,
               const PlanCacheCallbacks<KeyType, CachedPlanType>* callbacks = nullptr) {
        invariant(why);
        invariant(cachedPlan);

        if (solns.empty()) {
            return Status(ErrorCodes::BadValue, "no solutions provided");
        }

        auto statsSize =
            stdx::visit([](auto&& stats) { return stats.candidatePlanStats.size(); }, why->stats);
        if (statsSize != solns.size()) {
            return Status(ErrorCodes::BadValue, "number of stats in decision must match solutions");
        }

        if (why->scores.size() != why->candidateOrder.size()) {
            return Status(ErrorCodes::BadValue,
                          "number of scores in decision must match viable candidates");
        }

        if (why->candidateOrder.size() + why->failedCandidates.size() != solns.size()) {
            return Status(
                ErrorCodes::BadValue,
                "the number of viable candidates plus the number of failed candidates must "
                "match the number of solutions");
        }

        auto newWorks = stdx::visit(
            visit_helper::Overloaded{[](const plan_ranker::StatsDetails& details) {
                                         return details.candidatePlanStats[0]->common.works;
                                     },
                                     [](const plan_ranker::SBEStatsDetails& details) {
                                         return calculateNumberOfReads(
                                             details.candidatePlanStats[0].get());
                                     }},
            why->stats);

        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        auto [queryHash, planCacheKey, isNewEntryActive, shouldBeCreated] = [&]() {
            if (internalQueryCacheDisableInactiveEntries.load()) {
                // All entries are always active.
                return std::make_tuple(key.queryHash(),
                                       key.planCacheKeyHash(),
                                       true /* isNewEntryActive  */,
                                       true /* shouldBeCreated  */);
            } else {
                auto oldEntryWithStatus = _cache.get(key);
                tassert(6007020,
                        "LRU store must get value or NoSuchKey error code",
                        oldEntryWithStatus.isOK() ||
                            oldEntryWithStatus.getStatus() == ErrorCodes::NoSuchKey);
                Entry* oldEntry =
                    oldEntryWithStatus.isOK() ? oldEntryWithStatus.getValue() : nullptr;

                const auto newState = getNewEntryState(
                    key,
                    oldEntry,
                    newWorks,
                    worksGrowthCoefficient.get_value_or(internalQueryCacheWorksGrowthCoefficient),
                    callbacks);

                // Avoid recomputing the hashes if we've got an old entry to grab them from.
                return oldEntry ? std::make_tuple(oldEntry->queryHash,
                                                  oldEntry->planCacheKey,
                                                  newState.shouldBeActive,
                                                  newState.shouldBeCreated)
                                : std::make_tuple(key.queryHash(),
                                                  key.planCacheKeyHash(),
                                                  newState.shouldBeActive,
                                                  newState.shouldBeCreated);
            }
        }();

        if (!shouldBeCreated) {
            return Status::OK();
        }

        auto newEntry(Entry::create(solns,
                                    std::move(why),
                                    std::move(cachedPlan),
                                    queryHash,
                                    planCacheKey,
                                    now,
                                    isNewEntryActive,
                                    newWorks,
                                    callbacks));

        [[maybe_unused]] auto numEvicted = _cache.add(key, newEntry.release());

        return Status::OK();
    }

    /**
     * Set a cache entry back to the 'inactive' state. Rather than completely evicting an entry
     * when the associated plan starts to perform poorly, we deactivate it, so that plans which
     * perform even worse than the one already in the cache may not easily take its place.
     */
    void deactivate(const KeyType& key) {
        if (internalQueryCacheDisableInactiveEntries.load()) {
            // This is a noop if inactive entries are disabled.
            return;
        }

        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        auto entry = _cache.get(key);
        if (!entry.isOK()) {
            tassert(6007021,
                    "Unexpected error code from LRU store",
                    entry.getStatus() == ErrorCodes::NoSuchKey);
            return;
        }
        tassert(6007022, "LRU store must get a value or an error code", entry.getValue());
        entry.getValue()->isActive = false;
    }

    /**
     * Look up the cached data access for the provided key. Circumvents the recalculation
     * of a plan cache key.
     *
     * The return value will provide the "state" of the cache entry, as well as the CachedSolution
     * for the query (if there is one).
     */
    GetResult get(const KeyType& key) const {
        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        auto entry = _cache.get(key);
        if (!entry.isOK()) {
            tassert(6007023,
                    "Unexpected error code from LRU store",
                    entry.getStatus() == ErrorCodes::NoSuchKey);
            return {CacheEntryState::kNotPresent, nullptr};
        }
        tassert(6007024, "LRU store must get a value or an error code", entry.getValue());

        auto state = entry.getValue()->isActive ? CacheEntryState::kPresentActive
                                                : CacheEntryState::kPresentInactive;
        return {state, std::make_unique<CachedPlanHolder<CachedPlanType>>(*entry.getValue())};
    }

    /**
     * If the cache entry exists and is active, return a CachedSolution. If the cache entry is
     * inactive, log a message and return a nullptr. If no cache entry exists, return a nullptr.
     */
    std::unique_ptr<CachedPlanHolder<CachedPlanType>> getCacheEntryIfActive(
        const KeyType& key) const {
        auto res = get(key);
        if (res.state == CacheEntryState::kPresentInactive) {
            log_detail::logInactiveCacheEntry(key.toString());
            return nullptr;
        }

        return std::move(res.cachedPlanHolder);
    }

    /**
     * Remove the entry with the 'key' from the cache. If there is no entry for the given key in
     * the cache, this call is a no-op.
     */
    void remove(const KeyType& key) {
        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        [[maybe_unused]] auto ret = _cache.remove(key);
    }

    /**
     * Remove *all* cached plans.  Does not clear index information.
     */
    void clear() {
        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        _cache.clear();
    }

    /**
     * Reset the cache with new cache size. If the cache size is set to a smaller value than before,
     * enough entries are evicted in order to ensure that the cache fits within the new budget.
     */
    void reset(size_t size) {
        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        [[maybe_unused]] auto numEvicted = _cache.reset(size);
    }

    /**
     * Returns a copy of a cache entry, looked up by the plan cache key.
     *
     * If there is no entry in the cache for the 'query', returns an error Status.
     */
    StatusWith<std::unique_ptr<Entry>> getEntry(const KeyType& key) const {
        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        auto entry = _cache.get(key);
        if (!entry.isOK()) {
            return entry.getStatus();
        }
        invariant(entry.getValue());

        return std::unique_ptr<Entry>(entry.getValue()->clone());
    }

    /**
     * Returns a vector of all cache entries.
     * Used by planCacheListQueryShapes and index_filter_commands_test.cpp.
     */
    std::vector<std::unique_ptr<Entry>> getAllEntries() const {
        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        std::vector<std::unique_ptr<Entry>> entries;

        for (auto&& cacheEntry : _cache) {
            auto entry = cacheEntry.second;
            entries.push_back(std::unique_ptr<Entry>(entry->clone()));
        }

        return entries;
    }

    /**
     * Returns the size of the cache.
     * Used for testing.
     */
    size_t size() const {
        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        return _cache.size();
    }

    /**
     * Iterates over the plan cache. For each entry, serializes the PlanCacheEntryBase according to
     * 'serializationFunc'. Returns a vector of all serialized entries which match 'filterFunc'.
     */
    std::vector<BSONObj> getMatchingStats(
        const std::function<BSONObj(const Entry&)>& serializationFunc,
        const std::function<bool(const BSONObj&)>& filterFunc) const {
        std::vector<BSONObj> results;
        stdx::lock_guard<Latch> cacheLock(_cacheMutex);

        for (auto&& cacheEntry : _cache) {
            const auto entry = cacheEntry.second;
            auto serializedEntry = serializationFunc(*entry);
            if (filterFunc(serializedEntry)) {
                results.push_back(serializedEntry);
            }
        }

        return results;
    }

private:
    struct NewEntryState {
        bool shouldBeCreated = false;
        bool shouldBeActive = false;
    };

    /**
     * Given a query, and an (optional) current cache entry for its shape ('oldEntry'), determine
     * whether:
     * - We should create a new entry
     * - The new entry should be marked 'active'
     */
    NewEntryState getNewEntryState(const KeyType& key,
                                   Entry* oldEntry,
                                   size_t newWorks,
                                   double growthCoefficient,
                                   const PlanCacheCallbacks<KeyType, CachedPlanType>* callbacks) {
        NewEntryState res;
        if (!oldEntry) {
            if (callbacks) {
                callbacks->onCreateInactiveCacheEntry(key, oldEntry, newWorks);
            }
            res.shouldBeCreated = true;
            res.shouldBeActive = false;
            return res;
        }

        if (oldEntry->isActive && newWorks <= oldEntry->works) {
            // The new plan did better than the currently stored active plan. This case may
            // occur if many MultiPlanners are run simultaneously.
            if (callbacks) {
                callbacks->onReplaceActiveCacheEntry(key, oldEntry, newWorks);
            }
            res.shouldBeCreated = true;
            res.shouldBeActive = true;
        } else if (oldEntry->isActive) {
            if (callbacks) {
                callbacks->onNoopActiveCacheEntry(key, oldEntry, newWorks);
            }
            // There is already an active cache entry with a lower works value.
            // We do nothing.
            res.shouldBeCreated = false;
        } else if (newWorks > oldEntry->works) {
            // This plan performed worse than expected. Rather than immediately overwriting the
            // cache, lower the bar to what is considered good performance and keep the entry
            // inactive.

            // Be sure that 'works' always grows by at least 1, in case its current
            // value and 'internalQueryCacheWorksGrowthCoefficient' are low enough that
            // the old works * new works cast to size_t is the same as the previous value of
            // 'works'.
            const double increasedWorks = std::max(
                oldEntry->works + 1u, static_cast<size_t>(oldEntry->works * growthCoefficient));

            if (callbacks) {
                callbacks->onIncreasingWorkValue(key, oldEntry, increasedWorks);
            }
            oldEntry->works = increasedWorks;

            // Don't create a new entry.
            res.shouldBeCreated = false;
        } else {
            // This plan performed just as well or better than we expected, based on the
            // inactive entry's works. We use this as an indicator that it's safe to
            // cache (as an active entry) the plan this query used for the future.
            if (callbacks) {
                callbacks->onPromoteCacheEntry(key, oldEntry, newWorks);
            }
            // We'll replace the old inactive entry with an active entry.
            res.shouldBeCreated = true;
            res.shouldBeActive = true;
        }

        return res;
    }

    LRUKeyValue<KeyType, PlanCacheEntryBase<CachedPlanType>, BudgetEstimator, KeyHasher> _cache;

    // Protects _cache.
    mutable Mutex _cacheMutex = MONGO_MAKE_LATCH("PlanCache::_cacheMutex");
};

}  // namespace mongo
