// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>

#include <benchmark/benchmark.h>

// IWYU pragma: no_include "libunwind-x86_64.h"

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/platform/compiler.h"
#include "mongo/util/stacktrace.h"

#if defined(MONGO_CONFIG_USE_LIBUNWIND)
#include <libunwind.h>  // IWYU pragma: keep
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
    param.f = [&] {
        ++i;
    };
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
