// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/memory_tracking/query_memory_load_shedding.h"

#include <array>
#include <cstdint>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

using query_memory_load_shedding_detail::shedProbability;

struct Args {
    int64_t rssBytes;
    int64_t memLimitBytes;
    int64_t opTrackedBytes;
    int32_t lowMarkPercent;
    int32_t highMarkPercent;
    int64_t sizeReferenceBytes;
    Milliseconds sinceLastCheck;
};

constexpr int64_t kMemLimit = 32LL << 30;  // 32 GiB
constexpr int64_t kSizeRef = 32LL << 20;   // 32 MiB
constexpr int32_t kLow = 70, kHigh = 90;

int64_t rssPct(int pct) {
    return kMemLimit * pct / 100;
}

// A spread that exercises the full probability pipeline (pressure strictly between the marks, mixed
// operation sizes and intervals) plus the two short-circuit endpoints (below low, above high).
const std::array<Args, 8> kInputs = {{
    {rssPct(75), kMemLimit, 1LL << 20, kLow, kHigh, kSizeRef, Milliseconds(100)},  // small op
    {rssPct(80), kMemLimit, 32LL << 20, kLow, kHigh, kSizeRef, Milliseconds(1)},   // reference size
    {rssPct(85), kMemLimit, 256LL << 20, kLow, kHigh, kSizeRef, Milliseconds(50)},  // 8x reference
    {rssPct(88), kMemLimit, 1LL << 30, kLow, kHigh, kSizeRef, Milliseconds(1000)},  // 32x reference
    {rssPct(78), kMemLimit, 64LL << 20, kLow, kHigh, kSizeRef, Milliseconds(250)},  // 2x reference
    {rssPct(82), kMemLimit, 16LL << 20, kLow, kHigh, kSizeRef, Milliseconds(500)},  // half
                                                                                    // reference
    {rssPct(65), kMemLimit, 32LL << 20, kLow, kHigh, kSizeRef, Milliseconds(100)},  // below low
                                                                                    // mark
    {rssPct(95), kMemLimit, 32LL << 20, kLow, kHigh, kSizeRef, Milliseconds(100)},  // above high
                                                                                    // mark
}};

void BM_ShedProbability(benchmark::State& state) {
    size_t i = 0;
    for (auto _ : state) {
        const Args& a = kInputs[i++ & 7u];
        double p = shedProbability(a.rssBytes,
                                   a.memLimitBytes,
                                   a.opTrackedBytes,
                                   a.lowMarkPercent,
                                   a.highMarkPercent,
                                   a.sizeReferenceBytes,
                                   a.sinceLastCheck);
        benchmark::DoNotOptimize(p);
    }
}
BENCHMARK(BM_ShedProbability);

}  // namespace
}  // namespace mongo
