/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/util/decimal_counter.h"

#include "mongo/base/string_data.h"
#include "mongo/util/itoa.h"

#include <cstdint>
#include <limits>
#include <string>

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
            benchmark::DoNotOptimize(StringData(++count));
        }
        items += state.range(0);
    }
    state.SetItemsProcessed(items);
}

void BM_decimalCounterPostInc(benchmark::State& state) {
    uint64_t items = 0;
    for (auto _ : state) {
        DecimalCounter<uint32_t> count(state.range(1));
        for (int i = state.range(0); i--;) {
            benchmark::ClobberMemory();
            benchmark::DoNotOptimize(StringData(count++));
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
            benchmark::DoNotOptimize(StringData(ItoA(++count)));
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
BENCHMARK(BM_decimalCounterPostInc)->Args({10000, 0})->Args({{10, nonzeroStart}});
BENCHMARK(BM_ItoACounter)->Args({10000, 0})->Args({{10, nonzeroStart}});
BENCHMARK(BM_ToStringCounter)->Args({10000, 0})->Args({{10, nonzeroStart}});

}  // namespace mongo
