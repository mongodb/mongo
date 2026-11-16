// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/query_stats_bm_util.h"

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

using query_stats_bm::Environment;
using query_stats_bm::insertDistinctFindShape;
using query_stats_bm::kEvictingStoreCacheSize;
using query_stats_bm::kLargeStoreSize;
using query_stats_bm::kSmallStoreSize;
using query_stats_bm::kStoreCacheSize;
using query_stats_bm::Operation;

/**
 * Times the write path: insert 'n' distinct entries into an empty store. 'forceEvictions' sizes the
 * store below the population to exercise eviction. 'pushdownEnabled' maintains the KeyHashIndex,
 * which adds the user-visible hash at insert and index pruning on evict.
 */
void BM_QueryStatsStoreInsert(benchmark::State& state, bool forceEvictions, bool pushdownEnabled) {
    const int n = state.range(0);
    const size_t cacheSize = forceEvictions ? kEvictingStoreCacheSize : kStoreCacheSize;

    Environment env(pushdownEnabled, cacheSize);
    Operation op(env);

    for (auto _ : state) {
        // Start each iteration from an empty store, so every insert is a miss and the evicting case
        // keeps evicting. Reinserting the same keys would instead replace the existing entries.
        state.PauseTiming();
        env.resetStore();
        state.ResumeTiming();

        for (int i = 0; i < n; ++i) {
            insertDistinctFindShape(op, i);
        }
    }
    state.SetItemsProcessed(state.iterations() * n);
    state.SetComplexityN(n);
}

// All cases sweep the shared small-to-large store sizes and differ only in the store cache size and
// whether the KeyHashIndex is maintained.
BENCHMARK_CAPTURE(BM_QueryStatsStoreInsert,
                  Steady_Scan,
                  /*forceEvictions*/ false,
                  /*pushdownEnabled*/ false)
    ->Range(kSmallStoreSize, kLargeStoreSize)
    ->Complexity();
BENCHMARK_CAPTURE(BM_QueryStatsStoreInsert,
                  Steady_Pushdown,
                  /*forceEvictions*/ false,
                  /*pushdownEnabled*/ true)
    ->Range(kSmallStoreSize, kLargeStoreSize)
    ->Complexity();
// The 1MiB store holds only a few hundred ~2KB entries, so every point is past capacity.
BENCHMARK_CAPTURE(BM_QueryStatsStoreInsert,
                  Evicting_Scan,
                  /*forceEvictions*/ true,
                  /*pushdownEnabled*/ false)
    ->Range(kSmallStoreSize, kLargeStoreSize)
    ->Complexity();
BENCHMARK_CAPTURE(BM_QueryStatsStoreInsert,
                  Evicting_Pushdown,
                  /*forceEvictions*/ true,
                  /*pushdownEnabled*/ true)
    ->Range(kSmallStoreSize, kLargeStoreSize)
    ->Complexity();

}  // namespace
}  // namespace mongo
