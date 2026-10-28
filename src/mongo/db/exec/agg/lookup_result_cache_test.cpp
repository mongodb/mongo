// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/lookup_result_cache.h"

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

#include <limits>
#include <string_view>
#include <vector>

namespace mongo::exec::agg {
using namespace std::literals::string_view_literals;

namespace {
constexpr size_t kUnboundedSize = std::numeric_limits<size_t>::max();
const ValueComparator kDefaultComparator{nullptr};

Value makeResults(std::vector<Value> results) {
    return Value(std::move(results));
}

const Value kEmptyResults = makeResults({});
}  // namespace

class LookupResultCacheTest : public unittest::Test {
protected:
    size_t trackedBytes() const {
        return static_cast<size_t>(_tracker.inUseTrackedMemoryBytes());
    }

    SimpleMemoryUsageTracker _tracker;
};

TEST_F(LookupResultCacheTest, ComputeKeyReturnsSingleValue) {
    LookupResultCache cache(kDefaultComparator, &_tracker, kUnboundedSize, kUnboundedSize);
    auto key = cache.computeKey(Document{{"a", 5}}, FieldPath("a"));
    ASSERT_TRUE(key);
    ASSERT_VALUE_EQ(*key, Value(5));
}

TEST_F(LookupResultCacheTest, ComputeKeyReturnsNullForMissingField) {
    LookupResultCache cache(kDefaultComparator, &_tracker, kUnboundedSize, kUnboundedSize);
    auto key = cache.computeKey(Document{{"other", 5}}, FieldPath("a"));
    ASSERT_TRUE(key);
    ASSERT_VALUE_EQ(*key, Value(BSONNULL));
}

TEST_F(LookupResultCacheTest, ComputeKeyReturnsNoneForArrayValue) {
    LookupResultCache cache(kDefaultComparator, &_tracker, kUnboundedSize, kUnboundedSize);
    auto key =
        cache.computeKey(Document{{"a", std::vector<Value>{Value(1), Value(2)}}}, FieldPath("a"));
    ASSERT_FALSE(key);
}

TEST_F(LookupResultCacheTest, ComputeKeyReturnsNoneForObjectValue) {
    LookupResultCache cache(kDefaultComparator, &_tracker, kUnboundedSize, kUnboundedSize);
    auto key = cache.computeKey(Document{{"a", Document{{"b", 1}}}}, FieldPath("a"));
    ASSERT_FALSE(key);
}

TEST_F(LookupResultCacheTest, ComputeKeyReturnsNoneForMultipleValuesAlongPath) {
    // 'a' is an array of subdocuments, so 'a.b' resolves to more than one value.
    LookupResultCache cache(kDefaultComparator, &_tracker, kUnboundedSize, kUnboundedSize);
    auto key = cache.computeKey(
        Document{{"a", std::vector<Value>{Value(Document{{"b", 1}}), Value(Document{{"b", 2}})}}},
        FieldPath("a.b"));
    ASSERT_FALSE(key);
}

TEST_F(LookupResultCacheTest, ComputeKeyReturnsNoneWhenCacheIsDisabled) {
    auto cache = LookupResultCache::createDisabled(kDefaultComparator);
    ASSERT_FALSE(cache.isEnabled());
    auto key = cache.computeKey(Document{{"a", 5}}, FieldPath("a"));
    ASSERT_FALSE(key);
}

TEST_F(LookupResultCacheTest, ComputeKeyReturnsNoneWhenDisabledAndLocalFieldPathIsUnset) {
    // A disabled cache must not dereference 'localFieldPath' at all: pipeline-only $lookup (no
    // localField/foreignField) always constructs a disabled cache and passes boost::none here.
    auto cache = LookupResultCache::createDisabled(kDefaultComparator);
    ASSERT_FALSE(cache.isEnabled());
    auto key = cache.computeKey(Document{{"a", 5}}, boost::none);
    ASSERT_FALSE(key);
}

TEST_F(LookupResultCacheTest, MissingKeyReturnsNullptr) {
    LookupResultCache cache(kDefaultComparator, &_tracker, kUnboundedSize, kUnboundedSize);
    ASSERT_FALSE(cache.get(Value(0)));
    ASSERT_FALSE(cache.hasKey(Value(0)));
}

TEST_F(LookupResultCacheTest, PutThenGetReturnsExactResults) {
    LookupResultCache cache(kDefaultComparator, &_tracker, kUnboundedSize, kUnboundedSize);
    cache.put(Value(0), makeResults({Value(1), Value(2)}), 0);

    auto cached = cache.get(Value(0));
    ASSERT_TRUE(cached);
    ASSERT_EQ(cached->getArrayLength(), 2U);
    ASSERT_VALUE_EQ(cached->getArray()[0], Value(1));
    ASSERT_VALUE_EQ(cached->getArray()[1], Value(2));
}

TEST_F(LookupResultCacheTest, EmptyResultVectorIsCachedAndDistinguishableFromMiss) {
    LookupResultCache cache(kDefaultComparator, &_tracker, kUnboundedSize, kUnboundedSize);
    cache.put(Value(0), kEmptyResults, 0);

    auto cached = cache.get(Value(0));
    ASSERT_TRUE(cached);
    ASSERT_EQ(cached->getArrayLength(), 0U);

    // A genuinely absent key is still a miss.
    ASSERT_FALSE(cache.get(Value(1)));
}

TEST_F(LookupResultCacheTest, EntryExceedingPerEntryCapIsNotCached) {
    LookupResultCache cache(kDefaultComparator,
                            &_tracker,
                            kUnboundedSize,
                            /* maxEntrySizeBytes */ 10);
    cache.put(Value(0), makeResults({Value(1)}), 100);

    ASSERT_FALSE(cache.get(Value(0)));
    ASSERT_EQ(cache.size(), 0U);
    ASSERT_EQ(cache.totalSizeBytes(), 0U);
}

TEST_F(LookupResultCacheTest, PerEntryCapAccountsForKeySizeNotJustResultsSize) {
    const size_t keySize = Value(0).getApproximateSize();
    // approxResultsSize alone is under the cap, but approxResultsSize + keySize is not.
    LookupResultCache cache(kDefaultComparator,
                            &_tracker,
                            kUnboundedSize,
                            /* maxEntrySizeBytes */ keySize);
    cache.put(Value(0), makeResults({Value(1)}), /* approxResultsSize */ keySize - 1);

    ASSERT_FALSE(cache.get(Value(0)));
    ASSERT_EQ(cache.size(), 0U);
    ASSERT_EQ(cache.totalSizeBytes(), 0U);
}

TEST_F(LookupResultCacheTest, ZeroMaxEntriesNeverCachesAnything) {
    LookupResultCache cache(kDefaultComparator, &_tracker, /* maxEntries */ 0, kUnboundedSize);
    cache.put(Value(0), makeResults({Value(1)}), 0);

    ASSERT_FALSE(cache.get(Value(0)));
    ASSERT_EQ(cache.size(), 0U);
    ASSERT_EQ(cache.totalSizeBytes(), 0U);
}

TEST_F(LookupResultCacheTest, TotalSizeBytesAccountsForKeySizeEvenWithEmptyResults) {
    LookupResultCache cache(kDefaultComparator, &_tracker, kUnboundedSize, kUnboundedSize);
    cache.put(Value(0), kEmptyResults, 0);

    // An empty-results entry is still charged for its key, so it counts toward reported usage.
    ASSERT_EQ(cache.totalSizeBytes(), Value(0).getApproximateSize());
}

TEST_F(LookupResultCacheTest, MemoryTrackerFollowsPutsAndEvictions) {
    {
        LookupResultCache cache(kDefaultComparator, &_tracker, /* maxEntries */ 1, kUnboundedSize);
        cache.put(Value(0), makeResults({Value(1)}), 100);
        ASSERT_GT(trackedBytes(), 0U);
        ASSERT_EQ(trackedBytes(), cache.totalSizeBytes());

        // Admitting a second, differently-sized entry evicts the first, so the tracker should be
        // credited for the evicted entry and charged only for the new one.
        cache.put(Value(1), makeResults({Value(2)}), 10);
        ASSERT_EQ(cache.size(), 1U);
        ASSERT_EQ(trackedBytes(), cache.totalSizeBytes());
    }
    // Destroying the cache releases whatever it was still holding.
    ASSERT_EQ(trackedBytes(), 0U);
}

TEST_F(LookupResultCacheTest, EvictsLeastRecentlyUsedWhenOverMaxEntries) {
    LookupResultCache cache(kDefaultComparator, &_tracker, /* maxEntries */ 2, kUnboundedSize);
    cache.put(Value(0), kEmptyResults, 0);
    cache.put(Value(1), kEmptyResults, 0);
    // Cache ordering (MRU -> LRU): {1, 0}. 2 entries, within the cap.
    ASSERT_TRUE(cache.hasKey(Value(0)));
    ASSERT_TRUE(cache.hasKey(Value(1)));

    // Inserting a third entry exceeds the 2-entry cap, so the LRU entry (0) is evicted -- even
    // though every entry has empty (zero-size) results.
    cache.put(Value(2), kEmptyResults, 0);
    ASSERT_FALSE(cache.hasKey(Value(0)));
    ASSERT_TRUE(cache.hasKey(Value(1)));
    ASSERT_TRUE(cache.hasKey(Value(2)));
    ASSERT_EQ(cache.size(), 2U);
}

TEST_F(LookupResultCacheTest, GetPromotesEntryToMostRecentlyUsed) {
    LookupResultCache cache(kDefaultComparator, &_tracker, /* maxEntries */ 2, kUnboundedSize);
    cache.put(Value(0), kEmptyResults, 0);
    cache.put(Value(1), kEmptyResults, 0);
    // Cache ordering (MRU -> LRU): {1, 0}.

    // Promote 0 to MRU: {0, 1}.
    ASSERT_TRUE(cache.get(Value(0)));

    // Inserting a third entry should now evict 1, the new LRU.
    cache.put(Value(2), kEmptyResults, 0);
    ASSERT_TRUE(cache.hasKey(Value(0)));
    ASSERT_FALSE(cache.hasKey(Value(1)));
    ASSERT_TRUE(cache.hasKey(Value(2)));
}

TEST_F(LookupResultCacheTest, HasKeyDoesNotAffectLruOrder) {
    LookupResultCache cache(kDefaultComparator, &_tracker, /* maxEntries */ 2, kUnboundedSize);
    cache.put(Value(0), kEmptyResults, 0);
    cache.put(Value(1), kEmptyResults, 0);
    // Cache ordering (MRU -> LRU): {1, 0}.

    ASSERT_TRUE(cache.hasKey(Value(0)));

    // 0 is still LRU since hasKey() does not promote, so it is evicted.
    cache.put(Value(2), kEmptyResults, 0);
    ASSERT_FALSE(cache.hasKey(Value(0)));
    ASSERT_TRUE(cache.hasKey(Value(1)));
    ASSERT_TRUE(cache.hasKey(Value(2)));
}

TEST_F(LookupResultCacheTest, DisablesItselfAfterWarmupOnLowHitRate) {
    // Warmup of 10 lookups; must sustain a >=50% hit rate to stay enabled.
    LookupResultCache cache(kDefaultComparator,
                            &_tracker,
                            kUnboundedSize,
                            kUnboundedSize,
                            /* warmupSize */ 10,
                            /* minHitPercent */ 50);
    ASSERT_TRUE(cache.isEnabled());

    // 10 misses on distinct keys -> 0% hit rate. On the 10th miss the warmup window closes and the
    // cache disables itself.
    for (int i = 0; i < 10; ++i) {
        ASSERT_FALSE(cache.get(Value(i)));
    }
    ASSERT_FALSE(cache.isEnabled());
}

TEST_F(LookupResultCacheTest, StaysEnabledOnHighHitRateAndLatchesDecision) {
    // Warmup of 5 lookups; must sustain a >=50% hit rate to stay enabled.
    LookupResultCache cache(kDefaultComparator,
                            &_tracker,
                            kUnboundedSize,
                            kUnboundedSize,
                            /* warmupSize */ 5,
                            /* minHitPercent */ 50);
    cache.put(Value(0), makeResults({Value(1)}), 0);

    // 4 hits, then 1 miss closes the warmup window at an 80% hit rate: stays enabled.
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(cache.get(Value(0)));
    }
    ASSERT_FALSE(cache.get(Value(99)));
    ASSERT_TRUE(cache.isEnabled());

    // The decision is latched: a subsequent run of pure misses does not flip it back to disabled.
    for (int i = 100; i < 200; ++i) {
        ASSERT_FALSE(cache.get(Value(i)));
    }
    ASSERT_TRUE(cache.isEnabled());
}

TEST_F(LookupResultCacheTest, DisabledCacheDoesNotAdmitNewEntries) {
    LookupResultCache cache(kDefaultComparator,
                            &_tracker,
                            kUnboundedSize,
                            kUnboundedSize,
                            /* warmupSize */ 2,
                            /* minHitPercent */ 100);
    cache.put(Value(0), makeResults({Value(1)}), 0);
    ASSERT_TRUE(cache.get(Value(0)));  // 1 hit

    // Miss closes the warmup window at 50% (< 100%), disabling the cache. Disabling also releases
    // any memory the cache was already holding, since a disabled cache never serves again.
    ASSERT_FALSE(cache.get(Value(1)));
    ASSERT_FALSE(cache.isEnabled());
    ASSERT_EQ(cache.size(), 0U);

    // put() on a disabled cache is a no-op: no new entry is admitted.
    cache.put(Value(2), makeResults({Value(3)}), 0);
    ASSERT_FALSE(cache.hasKey(Value(2)));
    ASSERT_EQ(cache.size(), 0U);
}

TEST_F(LookupResultCacheTest, DisablingReleasesHeldMemory) {
    LookupResultCache cache(kDefaultComparator,
                            &_tracker,
                            kUnboundedSize,
                            kUnboundedSize,
                            /* warmupSize */ 0,
                            /* minHitPercent */ 0,
                            /* fastBailoutMisses */ 3);
    cache.put(Value(0), makeResults({Value(1)}), 0);
    cache.put(Value(1), makeResults({Value(2)}), 0);
    ASSERT_GT(cache.totalSizeBytes(), 0U);
    ASSERT_EQ(trackedBytes(), cache.totalSizeBytes());

    // 3 consecutive misses trip the fast bailout, disabling the cache and releasing everything it
    // was holding -- including the entries put() before the bailout tripped.
    ASSERT_FALSE(cache.get(Value(100)));
    ASSERT_FALSE(cache.get(Value(101)));
    ASSERT_FALSE(cache.get(Value(102)));
    ASSERT_FALSE(cache.isEnabled());
    ASSERT_EQ(cache.size(), 0U);
    ASSERT_EQ(cache.totalSizeBytes(), 0U);
    ASSERT_EQ(trackedBytes(), 0U);
}

TEST_F(LookupResultCacheTest, ZeroWarmupSizeNeverDisables) {
    // warmupSize 0 turns adaptive backoff off entirely, even with an impossible hit-rate floor.
    LookupResultCache cache(kDefaultComparator,
                            &_tracker,
                            kUnboundedSize,
                            kUnboundedSize,
                            /* warmupSize */ 0,
                            /* minHitPercent */ 100);
    for (int i = 0; i < 100; ++i) {
        ASSERT_FALSE(cache.get(Value(i)));
    }
    ASSERT_TRUE(cache.isEnabled());
}

TEST_F(LookupResultCacheTest, FastBailoutDisablesAfterConsecutiveMissStreak) {
    // No warmup/hit-rate backoff configured, only the fast bailout: 5 consecutive misses disable
    // the cache immediately, without waiting for any warmup window to close.
    LookupResultCache cache(kDefaultComparator,
                            &_tracker,
                            kUnboundedSize,
                            kUnboundedSize,
                            /* warmupSize */ 0,
                            /* minHitPercent */ 0,
                            /* fastBailoutMisses */ 5);
    for (int i = 0; i < 4; ++i) {
        ASSERT_FALSE(cache.get(Value(i)));
        ASSERT_TRUE(cache.isEnabled());
    }
    ASSERT_FALSE(cache.get(Value(4)));
    ASSERT_FALSE(cache.isEnabled());
}

TEST_F(LookupResultCacheTest, FastBailoutStreakResetsOnHit) {
    LookupResultCache cache(kDefaultComparator,
                            &_tracker,
                            kUnboundedSize,
                            kUnboundedSize,
                            /* warmupSize */ 0,
                            /* minHitPercent */ 0,
                            /* fastBailoutMisses */ 3);
    cache.put(Value(0), makeResults({Value(1)}), 0);

    // 2 misses, then a hit resets the streak, so a further run of 2 misses does not trip the
    // 3-miss bailout.
    ASSERT_FALSE(cache.get(Value(100)));
    ASSERT_FALSE(cache.get(Value(101)));
    ASSERT_TRUE(cache.get(Value(0)));
    ASSERT_FALSE(cache.get(Value(102)));
    ASSERT_FALSE(cache.get(Value(103)));
    ASSERT_TRUE(cache.isEnabled());
}

TEST_F(LookupResultCacheTest, ZeroFastBailoutMissesNeverTriggersBailout) {
    LookupResultCache cache(kDefaultComparator,
                            &_tracker,
                            kUnboundedSize,
                            kUnboundedSize,
                            /* warmupSize */ 0,
                            /* minHitPercent */ 0,
                            /* fastBailoutMisses */ 0);
    for (int i = 0; i < 100; ++i) {
        ASSERT_FALSE(cache.get(Value(i)));
    }
    ASSERT_TRUE(cache.isEnabled());
}

TEST_F(LookupResultCacheTest, FastBailoutDoesNotFireOnceWarmupDecisionIsLatched) {
    // Warmup closes on the 2nd lookup (a miss) at a 50% hit rate, which clears the 50% floor and
    // latches "stay enabled" -- the decision is only (re-)evaluated on a miss, so this requires at
    // least one miss to actually close the warmup window.
    LookupResultCache cache(kDefaultComparator,
                            &_tracker,
                            kUnboundedSize,
                            kUnboundedSize,
                            /* warmupSize */ 2,
                            /* minHitPercent */ 50,
                            /* fastBailoutMisses */ 3);
    cache.put(Value(0), makeResults({Value(1)}), 0);
    ASSERT_TRUE(cache.get(Value(0)));
    ASSERT_FALSE(cache.get(Value(999)));
    ASSERT_TRUE(cache.isEnabled());

    // The decision is already latched, so a subsequent miss streak longer than the fast bailout
    // threshold does not disable the cache.
    for (int i = 0; i < 10; ++i) {
        ASSERT_FALSE(cache.get(Value(i + 1)));
    }
    ASSERT_TRUE(cache.isEnabled());
}

TEST_F(LookupResultCacheTest, CacheKeysRespectCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    ValueComparator comparator{&collator};
    LookupResultCache cache(comparator, &_tracker, kUnboundedSize, kUnboundedSize);

    cache.put(Value("foo"sv), makeResults({Value(1)}), 0);

    auto cached = cache.get(Value("FOO"sv));
    ASSERT_TRUE(cached);
    ASSERT_EQ(cached->getArrayLength(), 1U);
    ASSERT_VALUE_EQ(cached->getArray()[0], Value(1));
}

}  // namespace mongo::exec::agg
