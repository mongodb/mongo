/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <benchmark/benchmark.h>

#include "mongo/stdx/unordered_map.h"
#include "mongo/util/immutable/unordered_map.h"

namespace mongo {

static void BM_absl_insert_op(benchmark::State& state) {
    stdx::unordered_map<int, int> map;

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

static void BM_absl_copy_op(benchmark::State& state) {
    stdx::unordered_map<int, int> map;

    int64_t i = 0;
    for (; i < state.range(0); i++) {
        map[i] = i;
    }

    stdx::unordered_map<int, int> mapCopy;
    for (auto _ : state) {
        mapCopy = map;
        benchmark::ClobberMemory();
    }
}

static void BM_absl_find_op(benchmark::State& state) {
    stdx::unordered_map<int, int> map;

    int64_t i = 0;
    for (; i < state.range(0); i++) {
        map[i] = i;
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(map.find(i - 1));
        benchmark::ClobberMemory();
    }
}

static void BM_absl_copy_and_insert_op(benchmark::State& state) {
    stdx::unordered_map<int, int> map;

    int64_t i = 0;
    for (; i < state.range(0); i++) {
        map[i] = i;
    }

    stdx::unordered_map<int, int> mapCopy;
    for (auto _ : state) {
        mapCopy = map;
        mapCopy[i] = i;
        benchmark::ClobberMemory();
    }
}

static void BM_immutable_insert_op(benchmark::State& state) {
    immutable::unordered_map<int, int> map;

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
    immutable::unordered_map<int, int> map;

    int64_t i = 0;
    for (; i < state.range(0); i++) {
        map = std::move(map).set(i, i);
    }

    immutable::unordered_map<int, int> mapCopy;
    for (auto _ : state) {
        mapCopy = map;
        benchmark::ClobberMemory();
    }
}

static void BM_immutable_find_op(benchmark::State& state) {
    immutable::unordered_map<int, int> map;

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
    immutable::unordered_map<int, int> map;

    int64_t i = 0;
    for (; i < state.range(0); i++) {
        map = std::move(map).set(i, i);
    }

    immutable::unordered_map<int, int> mapCopy;
    for (auto _ : state) {
        mapCopy = map.set(i, i);
        benchmark::ClobberMemory();
    }
}

// Run with varying container sizes: [ 8, 16, 32, 64, 128, 256, 512, 1024, 2k, 4k, 8k ].
BENCHMARK(BM_absl_insert_op)->RangeMultiplier(2)->Range(8, 8 << 10);
BENCHMARK(BM_immutable_insert_op)->RangeMultiplier(2)->Range(8, 8 << 10);

BENCHMARK(BM_absl_copy_op)->RangeMultiplier(2)->Range(8, 8 << 10);
BENCHMARK(BM_immutable_copy_op)->RangeMultiplier(2)->Range(8, 8 << 10);

BENCHMARK(BM_absl_find_op)->RangeMultiplier(2)->Range(8, 8 << 10);
BENCHMARK(BM_immutable_find_op)->RangeMultiplier(2)->Range(8, 8 << 10);

BENCHMARK(BM_absl_copy_and_insert_op)->RangeMultiplier(2)->Range(8, 8 << 10);
BENCHMARK(BM_immutable_copy_and_insert_op)->RangeMultiplier(2)->Range(8, 8 << 10);
}  // namespace mongo
