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

#include "mongo/db/catalog/util/partitioned.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/query/lru_key_value.h"
#include "mongo/db/query/plan_cache_callbacks.h"
#include "mongo/db/query/plan_cache_debug_info.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/container_size_helper.h"

namespace mongo {
class QuerySolution;
struct QuerySolutionNode;

template <class CachedPlanType, class DebugInfoType>
class PlanCacheEntryBase;

/**
 * Tracks the approximate cumulative size of the plan cache entries across all the collections.
 */
extern CounterMetric planCacheTotalSizeEstimateBytes;

/**
 * Information returned from a get(...) query.
 */
template <class CachedPlanType, class DebugInfoType>
class CachedPlanHolder {
private:
    CachedPlanHolder(const CachedPlanHolder&) = delete;
    CachedPlanHolder& operator=(const CachedPlanHolder&) = delete;

public:
    CachedPlanHolder(const PlanCacheEntryBase<CachedPlanType, DebugInfoType>& entry)
        : cachedPlan(entry.cachedPlan->clone()),
          decisionWorks(entry.works),
          debugInfo(entry.debugInfo) {}

    /**
     * Indicates whether or not the cached plan is pinned to cache.
     */
    bool isPinned() const {
        return !decisionWorks;
    }

    // A cached plan that can be used to reconstitute the complete execution plan from cache.
    std::unique_ptr<CachedPlanType> cachedPlan;

    // The number of work cycles taken to decide on a winning plan when the plan was first
    // cached. The value of boost::none indicates that the plan is pinned to the cache and
    // is not subject to replanning.
    const boost::optional<size_t> decisionWorks;

    // Per-plan cache entry information that is used for debugging purpose. Shared across all plans
    // recovered from the same cached entry.
    const std::shared_ptr<const DebugInfoType> debugInfo;
};

/**
 * Used by the cache to track entries and their performance over time.
 * Also used by the plan cache commands to display plan cache state.
 */
template <class CachedPlanType, class DebugInfoType>
class PlanCacheEntryBase {
public:
    using Entry = PlanCacheEntryBase<CachedPlanType, DebugInfoType>;

    static std::unique_ptr<Entry> create(std::unique_ptr<CachedPlanType> cachedPlan,
                                         uint32_t queryHash,
                                         uint32_t planCacheKey,
                                         uint32_t indexFilterKey,
                                         Date_t timeOfCreation,
                                         bool isActive,
                                         size_t works,
                                         DebugInfoType debugInfo) {
        // If the cumulative size of the plan caches is estimated to remain within a predefined
        // threshold, then then include additional debug info which is not strictly necessary for
        // the plan cache to be functional. Once the cumulative plan cache size exceeds this
        // threshold, omit this debug info as a heuristic to prevent plan cache memory consumption
        // from growing too large.
        bool includeDebugInfo = planCacheTotalSizeEstimateBytes.get() <
            internalQueryCacheMaxSizeBytesBeforeStripDebugInfo.load();

        // The stripping logic does not apply to SBE's debugging info as "DebugInfoSBE" is not
        // expected to be huge and is required to build a PlanExplainerSBE for the executor.
        if constexpr (std::is_same_v<DebugInfoType, plan_cache_debug_info::DebugInfoSBE>) {
            includeDebugInfo = true;
        }

        std::shared_ptr<const DebugInfoType> debugInfoOpt;
        if (includeDebugInfo) {
            debugInfoOpt = std::make_shared<const DebugInfoType>(std::move(debugInfo));
        }

        return std::unique_ptr<Entry>(new Entry(std::move(cachedPlan),
                                                timeOfCreation,
                                                queryHash,
                                                planCacheKey,
                                                indexFilterKey,
                                                isActive,
                                                works,
                                                std::move(debugInfoOpt)));
    }

    /**
     * Create a cache entry without a plan ranking decision. Such entries contain plans for which
     * there are no alternatives. As a result, these plans are pinned to the cache and are always
     * active.
     */
    static std::unique_ptr<Entry> createPinned(std::unique_ptr<CachedPlanType> cachedPlan,
                                               uint32_t queryHash,
                                               uint32_t planCacheKey,
                                               uint32_t indexFilterKey,
                                               Date_t timeOfCreation,
                                               DebugInfoType debugInfo) {
        return std::unique_ptr<Entry>(
            new Entry(std::move(cachedPlan),
                      timeOfCreation,
                      queryHash,
                      planCacheKey,
                      indexFilterKey,
                      true,         // isActive
                      boost::none,  // decisionWorks
                      std::make_shared<const DebugInfoType>(std::move(debugInfo))));
    }

    ~PlanCacheEntryBase() {
        planCacheTotalSizeEstimateBytes.decrement(estimatedEntrySizeBytes);
    }

    /**
     * Indicates whether or not the cache entry is pinned to cache. Pinned entries are always active
     * and are not subject to replanning.
     */
    bool isPinned() const {
        return !works;
    }

    /**
     * Make a copy of this plan cache entry. For all members a deep copy will be made, apart from
     * 'debugInfo' which is shared among all clone entries.
     */
    std::unique_ptr<Entry> clone() const {
        return std::unique_ptr<Entry>(new Entry(cachedPlan->clone(),
                                                timeOfCreation,
                                                queryHash,
                                                planCacheKey,
                                                indexFilterKey,
                                                isActive,
                                                works,
                                                debugInfo));
    }

    std::string debugString() const {
        StringBuilder builder;
        builder << "(";
        builder << "queryHash: " << queryHash;
        builder << "; planCacheKey: " << planCacheKey;
        if (debugInfo) {
            builder << "; " << debugInfo->debugString();
        }
        builder << "; timeOfCreation: " << timeOfCreation.toString() << ")";
        return builder.str();
    }

    // A cached plan that can be used to reconstitute the complete execution plan from cache.
    const std::unique_ptr<const CachedPlanType> cachedPlan;

    const Date_t timeOfCreation;

    // Hash of the cache key. Intended as an identifier for the query shape in logs and other
    // diagnostic output.
    const uint32_t queryHash;

    // Hash of the "stable" cache key, which is the same regardless of what indexes are around.
    const uint32_t planCacheKey;

    // Hash of the index filter key, which is used to match the query against index filters. This
    // type of key is encoded by filter, sort, projection and user-defined collation.
    const uint32_t indexFilterKey;

    // Whether or not the cache entry is active. Inactive cache entries should not be used for
    // planning.
    bool isActive = false;

    // The number of "works" required for a plan to run on this shape before it becomes
    // active. This value is also used to determine the number of works necessary in order to
    // trigger a replan. Running a query of the same shape while this cache entry is inactive may
    // cause this value to be increased.
    //
    // If boost::none the cached entry is pinned to cached. Pinned entries are always active
    // and are not subject to replanning.
    const boost::optional<size_t> works;

    // Optional debug info containing plan cache entry information that is used strictly as
    // debug information. Read-only and shared between all plans recovered from this entry.
    const std::shared_ptr<const DebugInfoType> debugInfo;

    // An estimate of the size in bytes of this plan cache entry. This is the "deep size",
    // calculated by recursively incorporating the size of owned objects, the objects that they in
    // turn own, and so on.
    const uint64_t estimatedEntrySizeBytes;

private:
    /**
     * All arguments constructor.
     */
    PlanCacheEntryBase(std::unique_ptr<CachedPlanType> cachedPlan,
                       Date_t timeOfCreation,
                       uint32_t queryHash,
                       uint32_t planCacheKey,
                       uint32_t indexFilterKey,
                       bool isActive,
                       boost::optional<size_t> works,
                       std::shared_ptr<const DebugInfoType> debugInfo)
        : cachedPlan(std::move(cachedPlan)),
          timeOfCreation(timeOfCreation),
          queryHash(queryHash),
          planCacheKey(planCacheKey),
          indexFilterKey(indexFilterKey),
          isActive(isActive),
          works(works),
          debugInfo(std::move(debugInfo)),
          estimatedEntrySizeBytes(_estimateObjectSizeInBytes()) {
        tassert(6108300, "A plan cache entry should never be empty", this->cachedPlan);
        tassert(6108301, "Pinned cache entry should always be active", !isPinned() || isActive);
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
          class DebugInfoType,
          class Partitioner,
          class KeyHasher = std::hash<KeyType>>
class PlanCacheBase {
private:
    PlanCacheBase(const PlanCacheBase&) = delete;
    PlanCacheBase& operator=(const PlanCacheBase&) = delete;

public:
    using Entry = PlanCacheEntryBase<CachedPlanType, DebugInfoType>;
    // The 'Value' being "std::shared_ptr<const Entry>" is because we allow readers to clone cache
    // entries out of the lock, therefore it is illegal to mutate the pieces of a cache entry that
    // can be cloned whether you are holding a lock or not.
    using Lru = LRUKeyValue<KeyType, std::shared_ptr<const Entry>, BudgetEstimator, KeyHasher>;

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
        std::unique_ptr<CachedPlanHolder<CachedPlanType, DebugInfoType>> cachedPlanHolder;
    };

    /**
     * Initialize plan cache with the total cache size in bytes and number of partitions.
     */
    explicit PlanCacheBase(size_t cacheSize, size_t numPartitions = 1)
        : _numPartitions(numPartitions) {
        invariant(numPartitions > 0);
        Lru lru{cacheSize / numPartitions};
        _partitionedCache = std::make_unique<Partitioned<Lru, Partitioner>>(numPartitions, lru);
    }

    ~PlanCacheBase() = default;

    /**
     * Tries to add 'cachedPlan' into the plan cache.
     *
     * Callers are responsible for passing the current time so that the time the plan cache entry
     * was created is stored in the plan cache.
     *
     * 'worksGrowthCoefficient' specifies what multiplier to use when growing the 'works' value of
     * an inactive cache entry.  If boost::none is provided, the function will use
     * 'internalQueryCacheWorksGrowthCoefficient'.
     *
     * A 'callbacks' argument should be provided to perform some custom actions when the state of
     * the plan cache or a plan cache entry has been changed. The 'callbacks' is also responsible
     * for constructing DebugInfo.
     *
     * If the mapping was set successfully, returns Status::OK(), even if it evicted another entry.
     */
    Status set(const KeyType& key,
               std::unique_ptr<CachedPlanType> cachedPlan,
               const plan_ranker::PlanRankingDecision& why,
               Date_t now,
               const PlanCacheCallbacks<KeyType, CachedPlanType, DebugInfoType>* callbacks,
               boost::optional<double> worksGrowthCoefficient = boost::none) {
        invariant(cachedPlan);

        if (why.scores.size() != why.candidateOrder.size()) {
            return Status(ErrorCodes::BadValue,
                          "number of scores in decision must match viable candidates");
        }

        auto newWorks = stdx::visit(
            visit_helper::Overloaded{[](const plan_ranker::StatsDetails& details) {
                                         return details.candidatePlanStats[0]->common.works;
                                     },
                                     [](const plan_ranker::SBEStatsDetails& details) {
                                         return calculateNumberOfReads(
                                             details.candidatePlanStats[0].get());
                                     }},
            why.stats);

        auto partition = _partitionedCache->lockOnePartition(key);
        auto [queryHash, planCacheKey, isNewEntryActive, shouldBeCreated, increasedWorks] = [&]() {
            if (internalQueryCacheDisableInactiveEntries.load()) {
                // All entries are always active.
                return std::make_tuple(key.queryHash(),
                                       key.planCacheKeyHash(),
                                       true /* isNewEntryActive  */,
                                       true /* shouldBeCreated  */,
                                       boost::optional<size_t>(boost::none));
            } else {
                auto oldEntryWithStatus = partition->get(key);
                tassert(6007020,
                        "LRU store must get value or NoSuchKey error code",
                        oldEntryWithStatus.isOK() ||
                            oldEntryWithStatus.getStatus() == ErrorCodes::NoSuchKey);
                auto oldEntry =
                    oldEntryWithStatus.isOK() ? oldEntryWithStatus.getValue()->second : nullptr;

                const auto newState = getNewEntryState(
                    key,
                    oldEntry.get(),
                    newWorks,
                    worksGrowthCoefficient.get_value_or(internalQueryCacheWorksGrowthCoefficient),
                    callbacks);

                // Avoid recomputing the hashes if we've got an old entry to grab them from.
                return oldEntry ? std::make_tuple(oldEntry->queryHash,
                                                  oldEntry->planCacheKey,
                                                  newState.shouldBeActive,
                                                  newState.shouldBeCreated,
                                                  newState.increasedWorks)
                                : std::make_tuple(key.queryHash(),
                                                  key.planCacheKeyHash(),
                                                  newState.shouldBeActive,
                                                  newState.shouldBeCreated,
                                                  newState.increasedWorks);
            }
        }();

        if (!shouldBeCreated) {
            return Status::OK();
        }

        // We use callback function here to build the 'DebugInfo' rather than pass in a constructed
        // DebugInfo for performance.
        //
        // Most of the time when either creating a new cache entry or replacing an old cache entry,
        // the 'works' value is based on the latest trial run. However, if the cache entry was
        // inactive and the latest trial required a higher works value, then we follow a special
        // formula for computing an 'increasedWorks' value.
        std::shared_ptr<Entry> newEntry = Entry::create(std::move(cachedPlan),
                                                        queryHash,
                                                        planCacheKey,
                                                        callbacks->getIndexFilterKeyHash(),
                                                        now,
                                                        isNewEntryActive,
                                                        increasedWorks ? *increasedWorks : newWorks,
                                                        callbacks->buildDebugInfo());

        partition->add(key, std::move(newEntry));
        return Status::OK();
    }

    /**
     * Adds a 'cachedPlan', resulting from a single QuerySolution, into the cache. A new cache entry
     * is always created and always active in this scenario.
     */
    void setPinned(const KeyType& key,
                   const uint32_t indexFilterKey,
                   std::unique_ptr<CachedPlanType> plan,
                   Date_t now,
                   DebugInfoType debugInfo) {
        invariant(plan);
        std::shared_ptr<Entry> entry = Entry::createPinned(std::move(plan),
                                                           key.queryHash(),
                                                           key.planCacheKeyHash(),
                                                           indexFilterKey,
                                                           now,
                                                           std::move(debugInfo));
        auto partition = _partitionedCache->lockOnePartition(key);
        // We're not interested in the number of evicted entries if the cache store exceeds the
        // budget after add(), so we just ignore the return value.
        partition->add(key, std::move(entry));
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

        auto partition = _partitionedCache->lockOnePartition(key);
        auto entry = partition->get(key);
        if (!entry.isOK()) {
            tassert(6007021,
                    "Unexpected error code from LRU store",
                    entry.getStatus() == ErrorCodes::NoSuchKey);
            return;
        }

        auto entryPtr = entry.getValue()->second;
        if (entryPtr->isActive == true) {
            std::shared_ptr<Entry> newEntry = entryPtr->clone();
            newEntry->isActive = false;
            partition->add(key, std::move(newEntry));
        }
    }

    /**
     * Look up the cached data access for the provided key. Circumvents the recalculation
     * of a plan cache key.
     *
     * The return value will provide the "state" of the cache entry, as well as the CachedSolution
     * for the query (if there is one).
     */
    GetResult get(const KeyType& key) const {
        std::shared_ptr<const Entry> entryPtr;
        CacheEntryState state;
        {
            auto partition = _partitionedCache->lockOnePartition(key);
            auto entry = partition->get(key);
            if (!entry.isOK()) {
                tassert(6007023,
                        "Unexpected error code from LRU store",
                        entry.getStatus() == ErrorCodes::NoSuchKey);
                return {CacheEntryState::kNotPresent, nullptr};
            }
            entryPtr = entry.getValue()->second;
            state = entryPtr->isActive ? CacheEntryState::kPresentActive
                                       : CacheEntryState::kPresentInactive;
        }
        // The purpose of cloning 'entry' after we release the lock is to allow multiple threads to
        // clone the same plan cache entry at once. 'entry' cannot be deleted by another thread even
        // if the plan cache is being concurrently modified by other threads because we are holding
        // a std::shared_ptr to this entry.
        tassert(6007024, "LRU store must get a value or an error code", entryPtr);

        return {state,
                std::make_unique<CachedPlanHolder<CachedPlanType, DebugInfoType>>(*entryPtr)};
    }

    /**
     * If the cache entry exists and is active, return a CachedSolution. If the cache entry is
     * inactive, log a message and return a nullptr. If no cache entry exists, return a nullptr.
     */
    std::unique_ptr<CachedPlanHolder<CachedPlanType, DebugInfoType>> getCacheEntryIfActive(
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
        _partitionedCache->erase(key);
    }

    /**
     * Remove all the entries for keys for which the predicate returns true. Return the number of
     * removed entries.
     */
    template <typename UnaryPredicate>
    size_t removeIf(UnaryPredicate predicate) {
        size_t nRemoved = 0;
        for (size_t partitionId = 0; partitionId < _numPartitions; ++partitionId) {
            auto lockedPartition = _partitionedCache->lockOnePartitionById(partitionId);
            nRemoved += lockedPartition->removeIf(predicate);
        }
        return nRemoved;
    }

    /**
     * Remove *all* cached plans.  Does not clear index information.
     */
    void clear() {
        _partitionedCache->clear();
    }

    /**
     * Reset total cache size. If the size is set to a smaller value than before, enough entries are
     * evicted in order to ensure that the cache fits within the new budget.
     */
    void reset(size_t cacheSize) {
        for (size_t partitionId = 0; partitionId < _numPartitions; ++partitionId) {
            auto lockedPartition = _partitionedCache->lockOnePartitionById(partitionId);
            lockedPartition->reset(cacheSize / _numPartitions);
        }
    }

    /**
     * Returns a copy of a cache entry, looked up by the plan cache key.
     *
     * If there is no entry in the cache for the 'query', returns an error Status.
     */
    StatusWith<std::unique_ptr<Entry>> getEntry(const KeyType& key) const {
        auto partition = _partitionedCache->lockOnePartition(key);
        auto entry = partition->get(key);
        if (!entry.isOK()) {
            return entry.getStatus();
        }
        invariant(entry.getValue()->second);

        return std::unique_ptr<Entry>(entry.getValue()->second->clone());
    }

    /**
     * Returns a vector of all cache entries. Does not guarantee a point-in-time view of the cache.
     */
    std::vector<std::unique_ptr<Entry>> getAllEntries() const {
        std::vector<std::unique_ptr<Entry>> entries;

        for (size_t partitionId = 0; partitionId < _numPartitions; ++partitionId) {
            auto lockedPartition = _partitionedCache->lockOnePartitionById(partitionId);

            for (auto&& [key, entry] : *lockedPartition) {
                entries.emplace_back(entry->clone());
            }
        }

        return entries;
    }

    /**
     * Returns the size of the cache.
     * Used for testing.
     */
    size_t size() const {
        return _partitionedCache->size();
    }

    /**
     * Iterates over the plan cache. For each entry, first filters according to the predicate
     * function 'cacheKeyFilterFunc', (Note that 'cacheKeyFilterFunc' could be empty, if so, we
     * don't filter by plan cache key.), then serializes the PlanCacheEntryBase according to
     * 'serializationFunc'. Returns a vector of all serialized entries which match 'filterFunc'.
     */
    std::vector<BSONObj> getMatchingStats(
        const std::function<bool(const KeyType&)>& cacheKeyFilterFunc,
        const std::function<BSONObj(const Entry&)>& serializationFunc,
        const std::function<bool(const BSONObj&)>& filterFunc) const {
        tassert(6033900,
                "serialization function and filter function are required when retrieving plan "
                "cache entries",
                serializationFunc && filterFunc);

        std::vector<BSONObj> results;

        for (size_t partitionId = 0; partitionId < _numPartitions; ++partitionId) {
            auto lockedPartition = _partitionedCache->lockOnePartitionById(partitionId);

            for (auto&& cacheEntry : *lockedPartition) {
                if (cacheKeyFilterFunc && !cacheKeyFilterFunc(cacheEntry.first)) {
                    continue;
                }
                const auto& entry = cacheEntry.second;
                auto serializedEntry = serializationFunc(*entry);
                if (filterFunc(serializedEntry)) {
                    results.push_back(serializedEntry);
                }
            }
        }

        return results;
    }

private:
    struct NewEntryState {
        bool shouldBeCreated = false;
        bool shouldBeActive = false;
        boost::optional<size_t> increasedWorks = boost::none;
    };

    /**
     * Given a query, and an (optional) current cache entry for its shape ('oldEntry'), determine
     * whether:
     * - We should create a new entry
     * - The new entry should be marked 'active'
     * - The new entry should update 'works' to the new value returned as 'increasedWorks'.
     */
    NewEntryState getNewEntryState(
        const KeyType& key,
        const Entry* oldEntry,
        size_t newWorks,
        double growthCoefficient,
        const PlanCacheCallbacks<KeyType, CachedPlanType, DebugInfoType>* callbacks) {
        NewEntryState res;
        if (!oldEntry) {
            if (callbacks) {
                callbacks->onCreateInactiveCacheEntry(key, oldEntry, newWorks);
            }
            res.shouldBeCreated = true;
            res.shouldBeActive = false;
            return res;
        }

        tassert(6108302,
                "Works value is not present in the old cache entry (is it a pinned entry?)",
                oldEntry->works);
        auto oldWorks = oldEntry->works.get();

        if (oldEntry->isActive && newWorks <= oldWorks) {
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
        } else if (newWorks > oldWorks) {
            // This plan performed worse than expected. Rather than immediately overwriting the
            // cache, lower the bar to what is considered good performance and keep the entry
            // inactive.

            // Be sure that 'works' always grows by at least 1, in case its current
            // value and 'internalQueryCacheWorksGrowthCoefficient' are low enough that
            // the old works * new works cast to size_t is the same as the previous value of
            // 'works'.
            const double increasedWorks =
                std::max(oldWorks + 1u, static_cast<size_t>(oldWorks * growthCoefficient));

            if (callbacks) {
                callbacks->onIncreasingWorkValue(key, oldEntry, increasedWorks);
            }

            // Create a new inactive cache entry with 'increasedWorks'.
            res.shouldBeCreated = true;
            res.increasedWorks.emplace(increasedWorks);
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

    std::size_t _numPartitions;
    std::unique_ptr<Partitioned<Lru, Partitioner>> _partitionedCache;
};

}  // namespace mongo
