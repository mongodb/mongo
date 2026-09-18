// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/partitioned_cache.h"

#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cstddef>
#include <vector>

using namespace mongo;

namespace {

struct TestBudgetEstimator {
    size_t operator()(const std::size_t&, const std::size_t&) {
        return 1;
    }
};

struct TestPartitioner {
    std::size_t operator()(const std::size_t key, const std::size_t nPartitions) const {
        return key % nPartitions;
    }
};

using TestCache = PartitionedCache<std::size_t,
                                   std::size_t,
                                   TestBudgetEstimator,
                                   TestPartitioner,
                                   NoopInsertionEvictionListener>;

// Identity accessor: the value is its own sort key.
uint64_t extractValue(const std::size_t& value) {
    return value;
}

void fillCache(TestCache& cache, size_t numEntries) {
    for (size_t i = 0; i < numEntries; ++i) {
        cache.put(i, i);
    }
}

// Extracts the values from topKCandidateEntries() results into a sorted vector for comparison.
// fillCache() makes every value equal to its key, so values identify entries.
std::vector<std::size_t> sortedCandidateValues(
    std::vector<std::pair<std::size_t, std::size_t>> candidates) {
    std::vector<std::size_t> values;
    for (auto& [key, value] : candidates) {
        values.push_back(value);
    }
    std::sort(values.begin(), values.end());
    return values;
}

TEST(PartitionedCacheTopKTest, DescendingSelectsLargestValues) {
    TestCache cache(100, 4);
    fillCache(cache, 100);

    auto candidates = cache.topKCandidateEntries(
        10, extractValue, [](uint64_t a, uint64_t b) { return a > b; }, [] {});

    std::vector<std::size_t> expected{90, 91, 92, 93, 94, 95, 96, 97, 98, 99};
    ASSERT(sortedCandidateValues(std::move(candidates)) == expected);
}

TEST(PartitionedCacheTopKTest, AscendingSelectsSmallestValues) {
    TestCache cache(100, 4);
    fillCache(cache, 100);

    auto candidates = cache.topKCandidateEntries(
        10, extractValue, [](uint64_t a, uint64_t b) { return a < b; }, [] {});

    std::vector<std::size_t> expected{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    ASSERT(sortedCandidateValues(std::move(candidates)) == expected);
}

TEST(PartitionedCacheTopKTest, SelectsBestCandidatesAcrossPartitions) {
    // With an odd number of partitions the largest values are spread unevenly across partitions.
    TestCache cache(100, 3);
    fillCache(cache, 100);

    auto candidates = cache.topKCandidateEntries(
        5, extractValue, [](uint64_t a, uint64_t b) { return a > b; }, [] {});

    std::vector<std::size_t> expected{95, 96, 97, 98, 99};
    ASSERT(sortedCandidateValues(std::move(candidates)) == expected);
}

TEST(PartitionedCacheTopKTest, FewerEntriesThanMaxCandidatesReturnsAll) {
    TestCache cache(100, 4);
    fillCache(cache, 7);

    auto candidates = cache.topKCandidateEntries(
        10, extractValue, [](uint64_t a, uint64_t b) { return a > b; }, [] {});

    std::vector<std::size_t> expected{0, 1, 2, 3, 4, 5, 6};
    ASSERT(sortedCandidateValues(std::move(candidates)) == expected);
}

TEST(PartitionedCacheTopKTest, ZeroMaxCandidatesReturnsEmpty) {
    TestCache cache(100, 4);
    fillCache(cache, 100);

    auto candidates = cache.topKCandidateEntries(
        0, extractValue, [](uint64_t a, uint64_t b) { return a > b; }, [] {});

    ASSERT(candidates.empty());
}

TEST(PartitionedCacheTopKTest, EmptyCacheReturnsEmpty) {
    TestCache cache(100, 4);

    auto candidates = cache.topKCandidateEntries(
        10, extractValue, [](uint64_t a, uint64_t b) { return a > b; }, [] {});

    ASSERT(candidates.empty());
}

TEST(PartitionedCacheTopKTest, CandidatesSurviveEvictionAfterScan) {
    TestCache cache(100, 4);
    fillCache(cache, 100);

    auto candidates = cache.topKCandidateEntries(
        10, extractValue, [](uint64_t a, uint64_t b) { return a > b; }, [] {});

    // Evict everything, including every selected candidate.
    cache.clear();

    // The candidates were copied during the scan, so none are lost.
    std::vector<std::size_t> expected{90, 91, 92, 93, 94, 95, 96, 97, 98, 99};
    ASSERT(sortedCandidateValues(std::move(candidates)) == expected);
}

TEST(PartitionedCacheTopKTest, CandidatesAreSnapshotsNotLiveReferences) {
    TestCache cache(100, 4);
    fillCache(cache, 100);

    auto candidates = cache.topKCandidateEntries(
        5, extractValue, [](uint64_t a, uint64_t b) { return a > b; }, [] {});

    // Update a selected candidate after the scan; the returned copy keeps its scanned value.
    cache.put(99, 0);
    for (auto& [key, value] : candidates) {
        if (key == 99) {
            ASSERT_EQUALS(value, 99);
            return;
        }
    }
    FAIL("expected key 99 among the candidates");
}

TEST(PartitionedCacheTopKTest, InterruptCheckRunsOncePerPartition) {
    TestCache cache(100, 4);
    fillCache(cache, 100);

    size_t numInterruptChecks = 0;
    auto candidates = cache.topKCandidateEntries(
        10,
        extractValue,
        [](uint64_t a, uint64_t b) { return a > b; },
        [&] { ++numInterruptChecks; });

    ASSERT_EQUALS(numInterruptChecks, 4);
    ASSERT_EQUALS(candidates.size(), 10);
}

TEST(PartitionedCacheTopKTest, InterruptCheckCanAbortScan) {
    TestCache cache(100, 4);
    fillCache(cache, 100);

    ASSERT_THROWS_CODE(cache.topKCandidateEntries(
                           10,
                           extractValue,
                           [](uint64_t a, uint64_t b) { return a > b; },
                           [] { uasserted(ErrorCodes::Interrupted, "interrupted"); }),
                       DBException,
                       ErrorCodes::Interrupted);
}

}  // namespace
