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

#include "mongo/util/duration.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/time_support.h"

#include <climits>
#include <memory>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

// Local testing determined that these parameter values drove the most lock contention, which is
// what we want to capture in this benchmark.
constexpr long long rateLimitedWorkTimeMicros = 5;
constexpr long long consistentWorkTimeMicros = 10;

constexpr long long numThreads = 256;

// Rate limit some fraction of the overall work for a request with a sliding window.
int requestWithSlidingWindow(RateLimiting& limit) {
    if (limit.handleRequestSlidingWindow()) {
        sleepmicros(rateLimitedWorkTimeMicros);
    }
    sleepmicros(consistentWorkTimeMicros);
    return 0;
}

// Represent a request that bypasses the rate limiter.
int requestUnlimited() {
    constexpr long long totalTime = rateLimitedWorkTimeMicros + consistentWorkTimeMicros;
    sleepmicros(totalTime);
    return 0;
}

// Represent a request without the rate limited work.
int requestDeactivated() {
    sleepmicros(consistentWorkTimeMicros);
    return 0;
}

// Benchmark sliding window rate limiting.
void BM_SlidingWindow(benchmark::State& state) {
    // The rate limiter needs a clock source passed in.
    static std::unique_ptr<ClockSource> clockSource;
    static std::unique_ptr<RateLimiting> rateLimit;

    // Initialize the rate limiter only on the first thread to start up.
    if (state.thread_index == 0) {
        clockSource = std::make_unique<SystemClockSource>();
        rateLimit =
            std::make_unique<RateLimiting>(state.range(0), Milliseconds(1), clockSource.get());
    }

    // Run the benchmark.
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(requestWithSlidingWindow(*rateLimit));
    }

    // Clean up the rate limiter when the benchmark is done.
    if (state.thread_index == 0) {
        rateLimit.reset();
        clockSource.reset();
    }
}

// "Control" benchmark that does not rate limit requests. In other words, the extra work is always
// done for every request. This benchmark can be thought of as the "goal" performance for the peak,
// or the highest rate limit in BM_SlidingWindow, to compare against.
void BM_Unlimited(benchmark::State& state) {
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(requestUnlimited());
    }
}
// Another control benchmark, where the extra work is never done for any request. This can be
// thought of as the goal performance for when rate limit equals 0.
void BM_Deactivated(benchmark::State& state) {
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(requestDeactivated());
    }
}

// Google microbenchmarks report time T (in nanoseconds) spent per operation. But at Mongo we are
// interested in total opereations performed per second. The former can easily be converted to the
// latter by diving 10^6 by T. Use this benchmark to determine the natural throughput of the
// operation. This can be compared to the rate limited benchmarks (BM_SlidingWindow) to determine
// the overhead of rate limiting. Looking at the percentage change in throughput between the control
// benchmarks and the rate limited benchmark, will indicate how much overhead is due to lock
// contention.
BENCHMARK(BM_Unlimited)->Threads(numThreads);

BENCHMARK(BM_Deactivated)->Threads(numThreads);

// Local testing has confirmed that the higher the rate limit, the worse the throughput. This makes
// sense as putting a higher upper bound on number of requests allowed in a given time period, means
// longer wait times for the lock.
BENCHMARK(BM_SlidingWindow)
    ->ArgName("rate limit")
    ->Arg(0)
    ->Arg(64)
    ->Arg(128)
    ->Arg(256)
    ->Arg(512)
    ->Arg(1024)
    ->Arg(2048)
    ->Arg(4816)
    ->Threads(numThreads);

}  // namespace
}  // namespace mongo
