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

#include "mongo/platform/basic.h"

#include <benchmark/benchmark.h>

#include "mongo/util/decimal_counter.h"
#include "mongo/util/itoa.h"

namespace mongo {

void BM_decimalCounterPreInc(benchmark::State& state) {
    uint64_t items = 0;
    for (auto _ : state) {
        DecimalCounter<uint32_t> count;
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
        DecimalCounter<uint32_t> count;
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
        uint32_t count = 0;
        for (int i = state.range(0); i--;) {
            benchmark::ClobberMemory();
            benchmark::DoNotOptimize(StringData(ItoA(++count)));
        }
        items += state.range(0);
    }
    state.SetItemsProcessed(items);
}

void BM_to_stringCounter(benchmark::State& state) {
    uint64_t items = 0;
    for (auto _ : state) {
        uint32_t count = 0;
        for (int i = state.range(0); i--;) {
            benchmark::ClobberMemory();
            benchmark::DoNotOptimize(std::to_string(++count));
        }
        items += state.range(0);
    }
    state.SetItemsProcessed(items);
}

BENCHMARK(BM_decimalCounterPreInc)->Arg(10000);
BENCHMARK(BM_decimalCounterPostInc)->Arg(10000);
BENCHMARK(BM_ItoACounter)->Arg(10000);
BENCHMARK(BM_to_stringCounter)->Arg(10000);

}  // namespace mongo
