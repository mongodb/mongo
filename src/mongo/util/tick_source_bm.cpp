// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/tick_source.h"

#include "mongo/util/system_tick_source.h"

#include <benchmark/benchmark.h>


namespace mongo {

static void BM_getTicks(benchmark::State& state) {
    auto tickSource = globalSystemTickSource();
    for (auto _ : state) {
        benchmark::DoNotOptimize(tickSource->getTicks());
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_getTicks);

}  // namespace mongo
