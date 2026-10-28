// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/field_path.h"

#include <cstddef>
#include <list>
#include <utility>
#include <vector>

namespace mongo::exec::agg {

/**
 * A collation-aware least-recently-used cache mapping a single classic $lookup localField value
 * to a Value holding the array of foreign results it produced. Bounded by a per-entry size cap,
 * entries larger than the cap are never cached and a maximum entry count enforced via LRU
 * eviction.
 *
 * A cached entry may legitimately be an empty vector (the key had no foreign matches); get()
 * distinguishes this from "not cached" via its return value.
 *
 * To avoid penalizing workloads it cannot help (e.g. high-cardinality joins where localField
 * values rarely repeat), the cache adaptively disables itself: after observing 'warmupSize'
 * lookups, if the hit rate is below 'minHitPercent' it stops caching for the remainder of the
 * query. Callers should consult isEnabled() and skip key extraction / get() / put() once it
 * returns false. A 'warmupSize' of 0 disables this backoff entirely.
 *
 * 'warmupSize' cannot help queries that finish before it is reached, so a second, independent
 * signal, 'fastBailoutMisses' consecutive misses with no intervening hit, disables the cache
 * immediately regardless of 'warmupSize'. A 'fastBailoutMisses' of 0 disables this fast path.
 *
 * Note: because a cached entry can outlive the foreign-collection read that produced it, a repeat
 * localField value served from the cache may reflect an earlier snapshot of the foreign
 * collection than a fresh read would e.g. under a storage engine that allows concurrent writes
 * to the foreign collection mid-query. This is an intentional tradeoff for performance.
 */
class LookupResultCache {
public:
    /**
     * Each cached entry is charged to 'memoryTracker' for as long as it stays cached, so the
     * tracker must outlive the cache. It may be null only for a cache that never admits entries
     * (see createDisabled()).
     */
    LookupResultCache(const ValueComparator& comparator,
                      SimpleMemoryUsageTracker* memoryTracker,
                      size_t maxEntries,
                      size_t maxEntrySizeBytes,
                      size_t warmupSize = 0,
                      size_t minHitPercent = 0,
                      size_t fastBailoutMisses = 0)
        : _memoryTracker(memoryTracker),
          _maxEntries(maxEntries),
          _maxEntrySizeBytes(maxEntrySizeBytes),
          _warmupSize(warmupSize),
          _minHitPercent(minHitPercent),
          _fastBailoutMisses(fastBailoutMisses),
          // Copied rather than held by reference: the ExpressionContext 'comparator' came from can
          // be replaced (e.g. by buildPipelineFromViewDefinition) after construction, which would
          // leave a reference-based comparator dangling.
          _comparator(comparator),
          _map(_comparator.makeUnorderedValueMap<EntryList::iterator>()) {}

    // '_map' hashes and compares keys through a pointer to this object's own '_comparator', so a
    // copied or moved instance would hash through a dangling pointer.
    LookupResultCache(const LookupResultCache&) = delete;
    LookupResultCache& operator=(const LookupResultCache&) = delete;
    LookupResultCache(LookupResultCache&&) = delete;
    LookupResultCache& operator=(LookupResultCache&&) = delete;

    /**
     * Returns a cache that is disabled from construction (isEnabled() always false, get()/put()
     * always no-ops). Used when a $lookup isn't eligible for caching at all (e.g. it has a user
     * pipeline, let variables, or targets a view), so callers can hold a single always-valid
     * LookupResultCache rather than an optional one.
     */
    static LookupResultCache createDisabled(const ValueComparator& comparator) {
        return LookupResultCache(DisabledTag{}, comparator);
    }

    /**
     * Same as createDisabled(const ValueComparator&), for callers with no comparator on hand,
     * a disabled cache never compares keys, so an arbitrary default one is fine.
     */
    static LookupResultCache createDisabled() {
        return createDisabled(ValueComparator());
    }

    /**
     * Returns the cache key for 'localFieldPath' in 'input', or boost::none if the cache isn't
     * serving (isEnabled() is false, checked before 'localFieldPath' is ever dereferenced, it is
     * unset precisely when the cache is permanently disabled, e.g. a pipeline-only $lookup with no
     * localField/foreignField), or the value isn't cacheable: a localField path resolving to more
     * than one value, or to a single Array/Object value (e.g. '[[1, 2]]', where the outer array
     * has one element that is itself an array). Mirrors makeMatchStageFromInput()'s value
     * semantics (missing -> null) for the single-value case.
     */
    boost::optional<Value> computeKey(const Document& input,
                                      const boost::optional<FieldPath>& localFieldPath);

    /**
     * Returns a pointer to the cached results for 'key', promoting the entry to
     * most-recently-used, or nullptr if 'key' is not cached.
     */
    const Value* get(const Value& key);

    /**
     * Returns false once the cache has adaptively disabled itself (see the class comment). Callers
     * should skip localField key extraction, get(), and put() when this returns false.
     */
    bool isEnabled() const {
        return _state != State::kDisabled;
    }

    /**
     * Returns true if 'key' is cached, without affecting LRU order. Always false once the cache is
     * disabled.
     */
    bool hasKey(const Value& key) const {
        return isEnabled() && _map.find(key) != _map.end();
    }

    /**
     * Caches 'results' for 'key' if 'approxResultsSize' is within the per-entry cap (otherwise a
     * no-op), evicting the least-recently-used entry if the cache is already at its maximum entry
     * count. 'results' is taken by reference so that the (potentially expensive) copy into the
     * cache entry only happens once we've decided to actually store it.
     */
    void put(const Value& key, const Value& results, size_t approxResultsSize);

    size_t size() const {
        return _entries.size();
    }

    size_t totalSizeBytes() const {
        return _totalSizeBytes;
    }

    size_t maxEntrySizeBytes() const {
        return _maxEntrySizeBytes;
    }

    // Marks the cache disabled for the remainder of the query and releases all memory it is
    // currently holding, since a disabled cache will never serve or admit another entry.
    void disable();

private:
    struct DisabledTag {};

    LookupResultCache(DisabledTag, const ValueComparator& comparator)
        : LookupResultCache(comparator,
                            /* memoryTracker */ nullptr,
                            /* maxEntries */ 0,
                            /* maxEntrySizeBytes */ 0) {
        disable();
    }

    struct Entry {
        Value key;
        Value results;
        // Holds the entry's approximate size, charged to '_memoryTracker' until it is destroyed.
        SimpleMemoryUsageToken memoryToken;
    };
    using EntryList = std::list<Entry>;

    void evictLru();

    // Evaluated once, when the warmup window closes: if the observed hit rate is below the
    // configured floor, disable the cache for the rest of the query; otherwise latch it as
    // permanently serving, so this check never re-runs. A no-op if adaptive backoff is turned off
    // (_warmupSize == 0) or a decision (Serving or Disabled) has already been latched.
    void maybeDisable();

    // Evaluated on every miss: an independent, cheaper-to-trip signal than maybeDisable() that
    // catches queries too short to ever reach 'warmupSize'. A no-op if a decision has already been
    // latched or the fast path is turned off (_fastBailoutMisses == 0).
    void maybeFastBailout();

    // kWarmup: still observing lookups, neither maybeDisable() nor maybeFastBailout() has decided
    // yet. kServing: maybeDisable() latched the cache as permanently enabled after warmup closed
    // with an acceptable hit rate. kDisabled: either mechanism tripped; the cache is off for the
    // rest of the query.
    enum class State { kWarmup, kServing, kDisabled };

    SimpleMemoryUsageTracker* _memoryTracker;
    size_t _maxEntries;
    size_t _maxEntrySizeBytes;
    size_t _warmupSize;
    size_t _minHitPercent;
    size_t _fastBailoutMisses;
    size_t _totalSizeBytes = 0;
    size_t _hits = 0;
    size_t _misses = 0;
    size_t _consecutiveMisses = 0;
    State _state = State::kWarmup;

    // Sorted from most-recently-used (front) to least-recently-used (back).
    EntryList _entries;
    ValueComparator _comparator;
    ValueUnorderedMap<EntryList::iterator> _map;
};

}  // namespace mongo::exec::agg
