// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/query_stats_bm_util.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

using query_stats_bm::assertPushdownFired;
using query_stats_bm::Environment;
using query_stats_bm::kLargeStoreSize;
using query_stats_bm::kSmallStoreSize;
using query_stats_bm::Operation;
using query_stats_bm::populateStore;
using query_stats_bm::runPipeline;

// The benchmark uses a single Environment for the whole binary: a second Environment would install
// a second global ServiceContext and destroy the first, which still has a live client. Instead the
// stores are cached and swapped in and out of this Environment as the benchmark moves between
// store sizes.
Environment& matchPushdownEnvironment(bool pushdownEnabled) {
    static Environment env(pushdownEnabled);
    return env;
}

// A populated store plus the user-visible hashes of the shapes it holds.
struct CachedStore {
    std::unique_ptr<query_stats::QueryStatsStoreManager> manager;
    std::vector<std::string> userVisibleHashes;
};

/**
 * Returns the user-visible hashes of the store of 'storeSize', populating it on first use and
 * installing it into the Environment so the pipeline under test resolves against it. A store of up
 * to 300K shapes is populated once rather than once per case.
 */
const std::vector<std::string>& loadStore(size_t storeSize, bool pushdownEnabled) {
    Environment& env = matchPushdownEnvironment(pushdownEnabled);
    static std::map<size_t, CachedStore> stores;
    static boost::optional<size_t> loaded;

    if (loaded == storeSize) {
        return stores[storeSize].userVisibleHashes;
    }
    if (loaded) {
        // Return the store currently installed to its slot before switching sizes.
        stores[*loaded].manager = env.takeStore();
    }
    auto& slot = stores[storeSize];
    if (!slot.manager) {
        env.resetStore();
        slot.userVisibleHashes = populateStore(env, static_cast<int>(storeSize));
        slot.manager = env.takeStore();
    }
    env.adoptStore(std::move(slot.manager));
    loaded = storeSize;
    return slot.userVisibleHashes;
}

/**
 * Varies the query stats store size for a single-target keyHash $match. With 'pushdownEnabled', the
 * target resolves through the KeyHashIndex in O(1) and only the matching entry is materialized;
 * otherwise the whole store is scanned and filtered.
 */
void BM_QueryStatsMatchPushdown_VaryStoreSize(benchmark::State& state,
                                              bool eqOrInPredicate,
                                              bool pushdownEnabled) {
    const int n = state.range(0);

    const auto& userVisibleHashes = loadStore(static_cast<size_t>(n), pushdownEnabled);
    std::vector<std::string> targets{userVisibleHashes.front()};

    Operation op(matchPushdownEnvironment(pushdownEnabled));
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

    const auto& userVisibleHashes = loadStore(kLargeStoreSize, pushdownEnabled);
    std::vector<std::string> targets(userVisibleHashes.begin(), userVisibleHashes.begin() + k);

    Operation op(matchPushdownEnvironment(pushdownEnabled));
    assertPushdownFired(op, targets, /*eqOrInPredicate*/ false, pushdownEnabled);
    const size_t expectedNumDocs = targets.size();
    for (auto _ : state) {
        const size_t numDocs = runPipeline(op, targets, /*eqOrInPredicate*/ false);
        benchmark::DoNotOptimize(numDocs);
        invariant(numDocs == expectedNumDocs);
    }
    state.SetComplexityN(k);
}

// The optimized path only: a $match on keyHash resolves through the KeyHashIndex instead of
// scanning the whole store.
BENCHMARK_CAPTURE(BM_QueryStatsMatchPushdown_VaryStoreSize,
                  Eq_Pushdown,
                  /*eqOrInPredicate*/ true,
                  /*pushdownEnabled*/ true)
    ->Range(kSmallStoreSize, kLargeStoreSize)
    ->Complexity();
BENCHMARK_CAPTURE(BM_QueryStatsMatchPushdown_VaryStoreSize,
                  In_Pushdown,
                  /*eqOrInPredicate*/ false,
                  /*pushdownEnabled*/ true)
    ->Range(kSmallStoreSize, kLargeStoreSize)
    ->Complexity();
BENCHMARK_CAPTURE(BM_QueryStatsMatchPushdown_VaryNumTargets, Pushdown, /*pushdownEnabled*/ true)
    ->Range(1, 1 << 8)
    ->Complexity();

}  // namespace
}  // namespace mongo
