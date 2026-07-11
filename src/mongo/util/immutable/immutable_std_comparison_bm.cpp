// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/immutable/map.h"

#include <cstdint>
#include <map>
#include <type_traits>
#include <utility>

#include <benchmark/benchmark.h>
#include <immer/detail/iterator_facade.hpp>
#include <immer/detail/util.hpp>

namespace mongo {

static void BM_std_insert_op(benchmark::State& state) {
    std::map<int, int> map;

    int64_t i = 0;
    for (; i < state.range(0); i++) {
        map[i] = i;
    }

    for (auto _ : state) {
        map[i] = i;
        i++;
        benchmark::ClobberMemory();
    }
}

static void BM_std_copy_op(benchmark::State& state) {
    std::map<int, int> map;

    int64_t i = 0;
    for (; i < state.range(0); i++) {
        map[i] = i;
    }

    std::map<int, int> mapCopy;
    for (auto _ : state) {
        mapCopy = map;
        benchmark::ClobberMemory();
    }
}

static void BM_std_find_op(benchmark::State& state) {
    std::map<int, int> map;

    int64_t i = 0;
    for (; i < state.range(0); i++) {
        map[i] = i;
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(map.find(i - 1));
        benchmark::ClobberMemory();
    }
}

static void BM_std_copy_and_insert_op(benchmark::State& state) {
    std::map<int, int> map;

    int64_t i = 0;
    for (; i < state.range(0); i++) {
        map[i] = i;
    }

    std::map<int, int> mapCopy;
    for (auto _ : state) {
        mapCopy = map;
        mapCopy[i] = i;
        benchmark::ClobberMemory();
    }
}

static void BM_immutable_insert_op(benchmark::State& state) {
    immutable::map<int, int> map;

    int64_t i = 0;
    for (; i < state.range(0); i++) {
        map = std::move(map).set(i, i);
    }

    for (auto _ : state) {
        map = std::move(map).set(i, i);
        i++;
        benchmark::ClobberMemory();
    }
}

static void BM_immutable_copy_op(benchmark::State& state) {
    immutable::map<int, int> map;

    int64_t i = 0;
    for (; i < state.range(0); i++) {
        map = std::move(map).set(i, i);
    }

    immutable::map<int, int> mapCopy;
    for (auto _ : state) {
        mapCopy = map;
        benchmark::ClobberMemory();
    }
}

static void BM_immutable_find_op(benchmark::State& state) {
    immutable::map<int, int> map;

    int64_t i = 0;
    for (; i < state.range(0); i++) {
        map = std::move(map).set(i, i);
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(map.find(i - 1));
        benchmark::ClobberMemory();
    }
}

static void BM_immutable_copy_and_insert_op(benchmark::State& state) {
    immutable::map<int, int> map;

    int64_t i = 0;
    for (; i < state.range(0); i++) {
        map = std::move(map).set(i, i);
    }

    immutable::map<int, int> mapCopy;
    for (auto _ : state) {
        mapCopy = map.set(i, i);
        benchmark::ClobberMemory();
    }
}

// Run with varying container sizes: [ 8, 16, 32, 64, 128, 256, 512, 1024, 2k, 4k, 8k ].
BENCHMARK(BM_std_insert_op)->RangeMultiplier(2)->Range(8, 8 << 10);
BENCHMARK(BM_immutable_insert_op)->RangeMultiplier(2)->Range(8, 8 << 10);

BENCHMARK(BM_std_copy_op)->RangeMultiplier(2)->Range(8, 8 << 10);
BENCHMARK(BM_immutable_copy_op)->RangeMultiplier(2)->Range(8, 8 << 10);

BENCHMARK(BM_std_find_op)->RangeMultiplier(2)->Range(8, 8 << 10);
BENCHMARK(BM_immutable_find_op)->RangeMultiplier(2)->Range(8, 8 << 10);

BENCHMARK(BM_std_copy_and_insert_op)->RangeMultiplier(2)->Range(8, 8 << 10);
BENCHMARK(BM_immutable_copy_and_insert_op)->RangeMultiplier(2)->Range(8, 8 << 10);
}  // namespace mongo
