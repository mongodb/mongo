// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/itoa.h"

#include <cstdint>

#include <benchmark/benchmark.h>
#include <fmt/compile.h>
#include <fmt/format.h>

namespace mongo {
namespace {

void BM_ItoA(benchmark::State& state) {
    std::uint64_t n = state.range(0);
    std::uint64_t items = 0;
    for (auto _ : state) {
        for (std::uint64_t i = 0; i < n; ++i) {
            benchmark::DoNotOptimize(ItoA(i));
            ++items;
        }
    }
    state.SetItemsProcessed(items);
}

void BM_ItoADigits(benchmark::State& state) {
    std::uint64_t n = state.range(0);
    std::uint64_t items = 0;

    std::uint64_t v = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        v = v * 10 + 9;
    }

    for (auto _ : state) {
        for (std::uint64_t i = 0; i < n; ++i) {
            benchmark::DoNotOptimize(ItoA(v));
            ++items;
        }
    }
    state.SetItemsProcessed(items);
}

BENCHMARK(BM_ItoA)
    ->Arg(1)
    ->Arg(10)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000)
    ->Arg(1000000)
    ->Arg(10000000);
BENCHMARK(BM_ItoADigits)->DenseRange(1, 20);

void BM_ItoAFmt(benchmark::State& state) {
    std::uint64_t n = state.range(0);
    std::uint64_t items = 0;
    for (auto _ : state) {
        for (std::uint64_t i = 0; i < n; ++i) {
            benchmark::DoNotOptimize(fmt::format("{}", i));
            ++items;
        }
    }
    state.SetItemsProcessed(items);
}

void BM_ItoADigitsFmt(benchmark::State& state) {
    std::uint64_t n = state.range(0);
    std::uint64_t items = 0;

    std::uint64_t v = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        v = v * 10 + 9;
    }

    for (auto _ : state) {
        for (std::uint64_t i = 0; i < n; ++i) {
            benchmark::DoNotOptimize(fmt::format("{}", v));
            ++items;
        }
    }
    state.SetItemsProcessed(items);
}

BENCHMARK(BM_ItoAFmt)
    ->Arg(1)
    ->Arg(10)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000)
    ->Arg(1000000)
    ->Arg(10000000);
BENCHMARK(BM_ItoADigitsFmt)->DenseRange(1, 20);

using namespace fmt::literals;
static constexpr auto cf = "{}"_cf;

void BM_ItoAFmtCf(benchmark::State& state) {
    std::uint64_t n = state.range(0);
    std::uint64_t items = 0;
    for (auto _ : state) {
        for (std::uint64_t i = 0; i < n; ++i) {
            benchmark::DoNotOptimize(fmt::format(cf, i));
            ++items;
        }
    }
    state.SetItemsProcessed(items);
}

void BM_ItoADigitsFmtCf(benchmark::State& state) {
    std::uint64_t n = state.range(0);
    std::uint64_t items = 0;

    std::uint64_t v = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        v = v * 10 + 9;
    }

    for (auto _ : state) {
        for (std::uint64_t i = 0; i < n; ++i) {
            benchmark::DoNotOptimize(fmt::format(cf, v));
            ++items;
        }
    }
    state.SetItemsProcessed(items);
}

BENCHMARK(BM_ItoAFmtCf)
    ->Arg(1)
    ->Arg(10)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000)
    ->Arg(1000000)
    ->Arg(10000000);
BENCHMARK(BM_ItoADigitsFmtCf)->DenseRange(1, 20);


}  // namespace
}  // namespace mongo
