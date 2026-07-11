// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/query_stats/rate_limiting.h"

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

constexpr long long numThreads = 256;

// This benchmark simulates a request that is handled by the rate limiter and
// checks the overhead of calling limiter.handle(), specifically to see how it handles lock
// contention. This BM does not include the overhead of the rate limiting logic itself, such as
// accessing the query stats store.

int handleRequest(RateLimiter& limiter) {
    if (limiter.handle()) {
        benchmark::DoNotOptimize(1);
    }

    benchmark::DoNotOptimize(0);
    return 0;
}

// Represent a request that bypasses the rate limiter.
int baselineRequest() {
    benchmark::DoNotOptimize(0);
    return 0;
}

// Benchmark sliding window rate limiting.
void BM_SlidingWindow(benchmark::State& state) {
    static RateLimiter limiter;
    // Initialize the rate limiter only on the first thread to start up.
    if (state.thread_index == 0) {
        limiter.configureWindowBased(state.range(0));
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(handleRequest(limiter));
    }
}

// Benchmark sample based rate limiting.
void BM_SampleBased(benchmark::State& state) {
    static RateLimiter limiter;
    // Initialize the rate limiter only on the first thread to start up.
    if (state.thread_index == 0) {
        limiter.configureSampleBased(state.range(0), 17 /* seed */);
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(handleRequest(limiter));
    }
}

void BM_Baseline(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(baselineRequest());
    }
}

BENCHMARK(BM_Baseline)->Threads(numThreads);

// Benchmark sliding window rate limiting.
// The overhead should be higher than sample based rate limiting
// but also consistent regardless of the rate limit due to the sliding window implementation.
BENCHMARK(BM_SlidingWindow)
    ->ArgName("rate limit in number of captured queries per second")
    ->Arg(0)
    ->Arg(100)  // 100 queries a second, default on server
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000)
    ->Threads(numThreads);

// Benchmark sample based rate limiting.
// The overhead should be constant regardless of the sample rate as
// it only uses a thread-local random number generator to determine whether the request should
// be handled or not.
BENCHMARK(BM_SampleBased)
    ->ArgName("rate limit in % denominated by 1000")
    ->Arg(0)
    ->Arg(10)  // 1% sample rate
    ->Arg(20)
    ->Arg(30)
    ->Arg(40)
    ->Arg(50)
    ->Arg(90)
    ->Arg(100)
    ->Threads(numThreads);

}  // namespace
}  // namespace mongo
