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
