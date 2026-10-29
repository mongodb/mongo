// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/decimal_counter.h"

#include "mongo/util/itoa.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {
auto nonzeroStart = std::numeric_limits<uint32_t>::max() / 2;
}  // namespace

void BM_decimalCounterPreInc(benchmark::State& state) {
    uint64_t items = 0;
    for (auto _ : state) {
        DecimalCounter<uint32_t> count(state.range(1));
        for (int i = state.range(0); i--;) {
            benchmark::ClobberMemory();
            benchmark::DoNotOptimize(std::string_view(++count));
        }
        items += state.range(0);
    }
    state.SetItemsProcessed(items);
}

void BM_ItoACounter(benchmark::State& state) {
    uint64_t items = 0;
    for (auto _ : state) {
        uint32_t count = state.range(1);
        for (int i = state.range(0); i--;) {
            benchmark::ClobberMemory();
            benchmark::DoNotOptimize(std::string_view(ItoA(++count)));
        }
        items += state.range(0);
    }
    state.SetItemsProcessed(items);
}

void BM_ToStringCounter(benchmark::State& state) {
    uint64_t items = 0;
    for (auto _ : state) {
        uint32_t count = state.range(1);
        for (int i = state.range(0); i--;) {
            benchmark::ClobberMemory();
            benchmark::DoNotOptimize(std::to_string(++count));
        }
        items += state.range(0);
    }
    state.SetItemsProcessed(items);
}

BENCHMARK(BM_decimalCounterPreInc)->Args({10000, 0})->Args({{10, nonzeroStart}});
BENCHMARK(BM_ItoACounter)->Args({10000, 0})->Args({{10, nonzeroStart}});
BENCHMARK(BM_ToStringCounter)->Args({10000, 0})->Args({{10, nonzeroStart}});

}  // namespace mongo
