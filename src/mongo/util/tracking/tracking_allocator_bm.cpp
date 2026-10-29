// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/tracking/allocator.h"

#include <benchmark/benchmark.h>

namespace mongo::tracking {

namespace {

constexpr size_t numIncsPerRead = 1000;

}  // namespace

class AllocatorFixture : public benchmark::Fixture {
protected:
    std::unique_ptr<AllocatorStats> _stats;
};

BENCHMARK_DEFINE_F(AllocatorFixture, counter)(benchmark::State& state) {
    if (state.thread_index == 0) {
        // Setup code
        // state.range(0) --> Number of partitions
        _stats = std::make_unique<AllocatorStats>(state.range(0));
    }

    for (auto _ : state) {
        for (size_t i = 0; i < numIncsPerRead; i++) {
            _stats->bytesAllocated(1);
        }
        benchmark::DoNotOptimize(_stats->allocated());
    }

    if (state.thread_index == 0) {
        // Teardown code
        state.counters["numIncrements"] = _stats->allocated();
    }
}

BENCHMARK_REGISTER_F(AllocatorFixture, counter)
    ->RangeMultiplier(2)
    ->Ranges({{1, 64}})
    ->ThreadRange(1, 64)
    ->UseRealTime();

}  // namespace mongo::tracking
