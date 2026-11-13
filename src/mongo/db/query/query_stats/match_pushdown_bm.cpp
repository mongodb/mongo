// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/query_stats_bm_util.h"

#include <string>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

using query_stats_bm::assertPushdownFired;
using query_stats_bm::Environment;
using query_stats_bm::kLargeStoreSize;
using query_stats_bm::kSmallStoreSize;
using query_stats_bm::Operation;
using query_stats_bm::populateStore;
using query_stats_bm::runPipeline;

/**
 * Varies the query stats store size for a single-target keyHash $match. With 'pushdownEnabled', the
 * target resolves through the KeyHashIndex in O(1) and only the matching entry is materialized;
 * otherwise the whole store is scanned and filtered.
 */
void BM_QueryStatsMatchPushdown_VaryStoreSize(benchmark::State& state,
                                              bool eqOrInPredicate,
                                              bool pushdownEnabled) {
    const int n = state.range(0);

    Environment env(pushdownEnabled);
    auto userVisibleHashes = populateStore(env, n);
    std::vector<std::string> targets{userVisibleHashes.front()};

    Operation op(env);
    assertPushdownFired(op, targets, eqOrInPredicate, pushdownEnabled);
    const size_t expectedNumDocs = targets.size();
    for (auto _ : state) {
        const size_t numDocs = runPipeline(op, targets, eqOrInPredicate);
        benchmark::DoNotOptimize(numDocs);
        invariant(numDocs == expectedNumDocs);
    }
    state.SetComplexityN(n);
}

/**
 * Varies the number of keyHash targets in a $in $match against a fixed-size store. With
 * 'pushdownEnabled', cost grows with the number of matched targets rather than the store size.
 */
void BM_QueryStatsMatchPushdown_VaryNumTargets(benchmark::State& state, bool pushdownEnabled) {
    const int k = state.range(0);

    Environment env(pushdownEnabled);
    auto userVisibleHashes = populateStore(env, static_cast<int>(kLargeStoreSize));
    std::vector<std::string> targets(userVisibleHashes.begin(), userVisibleHashes.begin() + k);

    Operation op(env);
    assertPushdownFired(op, targets, /*eqOrInPredicate*/ false, pushdownEnabled);
    const size_t expectedNumDocs = targets.size();
    for (auto _ : state) {
        const size_t numDocs = runPipeline(op, targets, /*eqOrInPredicate*/ false);
        benchmark::DoNotOptimize(numDocs);
        invariant(numDocs == expectedNumDocs);
    }
    state.SetComplexityN(k);
}

BENCHMARK_CAPTURE(BM_QueryStatsMatchPushdown_VaryStoreSize,
                  Eq_Scan,
                  /*eqOrInPredicate*/ true,
                  /*pushdownEnabled*/ false)
    ->Range(kSmallStoreSize, kLargeStoreSize)
    ->Complexity();
BENCHMARK_CAPTURE(BM_QueryStatsMatchPushdown_VaryStoreSize,
                  Eq_Pushdown,
                  /*eqOrInPredicate*/ true,
                  /*pushdownEnabled*/ true)
    ->Range(kSmallStoreSize, kLargeStoreSize)
    ->Complexity();
BENCHMARK_CAPTURE(BM_QueryStatsMatchPushdown_VaryStoreSize,
                  In_Scan,
                  /*eqOrInPredicate*/ false,
                  /*pushdownEnabled*/ false)
    ->Range(kSmallStoreSize, kLargeStoreSize)
    ->Complexity();
BENCHMARK_CAPTURE(BM_QueryStatsMatchPushdown_VaryStoreSize,
                  In_Pushdown,
                  /*eqOrInPredicate*/ false,
                  /*pushdownEnabled*/ true)
    ->Range(kSmallStoreSize, kLargeStoreSize)
    ->Complexity();
BENCHMARK_CAPTURE(BM_QueryStatsMatchPushdown_VaryNumTargets, Scan, /*pushdownEnabled*/ false)
    ->Range(1, 1 << 8)
    ->Complexity();
BENCHMARK_CAPTURE(BM_QueryStatsMatchPushdown_VaryNumTargets, Pushdown, /*pushdownEnabled*/ true)
    ->Range(1, 1 << 8)
    ->Complexity();

}  // namespace
}  // namespace mongo
