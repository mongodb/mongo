// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/clock_source.h"

#include "mongo/util/duration.h"
#include "mongo/util/fast_clock_source_factory.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/system_clock_source.h"

#include <memory>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

/**
 * Benchmark calls to the now() method of a clock source. With an argument of 0,
 * tests the system clock source, and with larger values, uses the FastClockSource
 * with a clock resolution of the specified number of milliseconds.
 *
 * All threads executing the benchmark use the same instance of the clock source,
 * to allow benchmarking to identify synchronization costs inside the now() method.
 */
void BM_ClockNow(benchmark::State& state) {
    static std::unique_ptr<ClockSource> clock;
    if (state.thread_index == 0) {
        if (state.range(0) > 0) {
            clock = FastClockSourceFactory::create(Milliseconds{state.range(0)});
        } else if (state.range(0) == 0) {
            clock = std::make_unique<SystemClockSource>();
        } else {
            state.SkipWithError("poll period must be non-negative");
            return;
        }
    }

    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(clock->now());
    }

    if (state.thread_index == 0) {
        clock.reset();
    }
}

BENCHMARK(BM_ClockNow)
    ->ThreadRange(1, ProcessInfo::getNumAvailableCores())
    ->ArgName("poll period")
    ->Arg(0)
    ->Arg(1)
    ->Arg(10);

}  // namespace
}  // namespace mongo
