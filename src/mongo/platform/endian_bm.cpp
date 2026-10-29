// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/platform/endian.h"

#include <cstdint>
#include <numeric>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

template <typename T>
void BM_NativeToBig(benchmark::State& state) {
    std::uint64_t items = 0;
    std::vector<T> in(1000);
    std::iota(in.begin(), in.end(), 0);
    for (auto _ : state) {
        for (auto& i : in)
            benchmark::DoNotOptimize(endian::nativeToBig(i));
        items += in.size();
    }
    state.SetItemsProcessed(items);
}

template <typename T>
void BM_NativeToLittle(benchmark::State& state) {
    std::uint64_t items = 0;
    std::vector<T> in(1000);
    std::iota(in.begin(), in.end(), 0);
    for (auto _ : state) {
        for (auto& i : in)
            benchmark::DoNotOptimize(endian::nativeToLittle(i));
        items += in.size();
    }
    state.SetItemsProcessed(items);
}

BENCHMARK_TEMPLATE(BM_NativeToBig, uint16_t);
BENCHMARK_TEMPLATE(BM_NativeToBig, uint32_t);
BENCHMARK_TEMPLATE(BM_NativeToBig, uint64_t);
BENCHMARK_TEMPLATE(BM_NativeToLittle, uint16_t);
BENCHMARK_TEMPLATE(BM_NativeToLittle, uint32_t);
BENCHMARK_TEMPLATE(BM_NativeToLittle, uint64_t);

}  // namespace
}  // namespace mongo
