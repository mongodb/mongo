/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "mongo/base/string_data.h"
#include "mongo/config.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/stacktrace.h"

#if defined(MONGO_CONFIG_USE_LIBUNWIND)
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#endif


namespace mongo {
namespace {

struct RecursionParam {
    std::function<void()> f;
    std::uint64_t n;
};

// Re-enters itself 'p.n' times to synthesize a deep call stack, then invokes `p.f()`.
MONGO_COMPILER_NOINLINE int recursionTest(RecursionParam& p, std::uint64_t i = 0) {
    if (i == p.n) {
        p.f();
    } else {
        recursionTest(p, i + 1);
    }
    return 0;
}

void BM_Baseline(benchmark::State& state) {
    size_t items = 0;
    RecursionParam param;
    size_t i = 0;
    param.n = state.range(0);
    param.f = [&] { ++i; };
    for (auto _ : state) {
        benchmark::DoNotOptimize(recursionTest(param));
        ++items;
    }
    state.SetItemsProcessed(items);
}
BENCHMARK(BM_Baseline)->Range(1, 100);

void BM_Print(benchmark::State& state) {
    size_t items = 0;
    RecursionParam param;
    std::ostringstream os;
    param.n = state.range(0);
    param.f = [&] {
        os.clear();
        printStackTrace(os);
        items += param.n;
    };
    for (auto _ : state) {
        benchmark::DoNotOptimize(recursionTest(param));
    }
    state.SetItemsProcessed(items);
}
BENCHMARK(BM_Print)->Range(1, 100);

#if defined(MONGO_CONFIG_USE_LIBUNWIND)
void BM_CursorSteps(benchmark::State& state) {
    size_t items = 0;
    RecursionParam param;
    std::ostringstream os;
    param.n = state.range(0);
    param.f = [&] {
        unw_context_t context;
        if (int r = unw_getcontext(&context); r < 0) {
            std::cerr << "unw_getcontext: " << unw_strerror(r) << "\n";
            return;
        }
        unw_cursor_t cursor;
        if (int r = unw_init_local(&cursor, &context); r < 0) {
            std::cerr << "unw_init_local: " << unw_strerror(r) << "\n";
            return;
        }
        while (true) {
            ++items;  // count each unw_step as an item
            if (int r = unw_step(&cursor); r < 0) {
                std::cerr << "unw_step: " << unw_strerror(r) << "\n";
                return;
            } else if (r == 0) {
                break;
            }
        }
    };
    for (auto _ : state) {
        benchmark::DoNotOptimize(recursionTest(param));
    }
    state.SetItemsProcessed(items);
}
BENCHMARK(BM_CursorSteps)->Range(1, 100);
#endif  // MONGO_CONFIG_USE_LIBUNWIND


}  // namespace
}  // namespace mongo
