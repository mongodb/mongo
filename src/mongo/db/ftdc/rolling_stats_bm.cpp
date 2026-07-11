// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/ftdc/rolling_stats.h"

#include "mongo/platform/random.h"
#include "mongo/util/clock_source_mock.h"

#include <benchmark/benchmark.h>

namespace mongo {
class RollingStatsBmHelper {
public:
    static int64_t sumFromWindowData(RollingStats& stats) {
        return stats._readWindowData().sum;
    }
};

namespace {

constexpr int64_t kMaxMeasurement = 10000;

// All the benchmarks are limited to 1000 iterations because they require significant un-timed setup
// (recording a bunch of records) which takes longer than the part that is measured by the benchmark
// (adding one more record, or getting stats).

// A value is recorded without having any existing values that are too old.
void BM_RecordNoPurge(benchmark::State& state) {
    int numRecords = state.range(0);
    PseudoRandom rand(1);
    for (auto _ : state) {
        state.PauseTiming();
        RollingStats stats;
        for (int i = 0; i < numRecords; ++i) {
            stats.record(rand.nextInt64(kMaxMeasurement));
        }
        int64_t to_record = rand.nextInt64(kMaxMeasurement);
        state.ResumeTiming();
        stats.record(to_record);
    }
}
BENCHMARK(BM_RecordNoPurge)->Range(1, 64000)->Iterations(1000);

// A value is recorded while all existing values are too old.
void BM_RecordWithPurge(benchmark::State& state) {
    int numRecords = state.range(0);
    PseudoRandom rand(1);
    ClockSourceMock clock;
    for (auto _ : state) {
        state.PauseTiming();
        RollingStats stats({.clock = &clock});
        for (int i = 0; i < numRecords; ++i) {
            stats.record(rand.nextInt64(kMaxMeasurement));
        }
        int64_t to_record = rand.nextInt64(kMaxMeasurement);
        clock.advance(Seconds(61));
        state.ResumeTiming();
        stats.record(to_record);
    }
}
BENCHMARK(BM_RecordWithPurge)->Range(1, 64000)->Iterations(1000);

void BM_GetStatsSingleTime(benchmark::State& state) {
    int numRecords = state.range(0);
    PseudoRandom rand(1);
    for (auto _ : state) {
        state.PauseTiming();
        RollingStats stats;
        for (int i = 0; i < numRecords; ++i) {
            stats.record(rand.nextInt64(kMaxMeasurement));
        }
        state.ResumeTiming();
        stats.getStats();
    }
}
BENCHMARK(BM_GetStatsSingleTime)->Range(1, 64000)->Iterations(1000);

void BM_GetStatsDifferentTimes(benchmark::State& state) {
    int numRecords = state.range(0);
    PseudoRandom rand(1);
    ClockSourceMock clock;
    // Spread the records out evenly over 1 min. Need to use Microseconds because eventually
    // numRecords will be greater than 60k, which is the number of millis in a minute.
    Microseconds timeIncrement = Microseconds(Seconds(60)) / numRecords;
    Microseconds elapsedTime(0);

    for (auto _ : state) {
        state.PauseTiming();
        RollingStats stats({.clock = &clock});
        for (int i = 0; i < numRecords; ++i) {
            stats.record(rand.nextInt64(kMaxMeasurement));

            elapsedTime += timeIncrement;
            if (elapsedTime > Milliseconds(1)) {
                Milliseconds elapsedMillis = duration_cast<Milliseconds>(elapsedTime);
                elapsedTime -= elapsedMillis;
                clock.advance(elapsedMillis);
            }
        }
        state.ResumeTiming();
        stats.getStats();
    }
}
BENCHMARK(BM_GetStatsDifferentTimes)->Range(1, 64000)->Iterations(1000);

// Calls the internal function exposed by the helper to track how much time the lock is held when
// computing stats, which should indicate the likelihood of this causing noticeable contention.
void BM_GetStatsLockedTimeSingleTime(benchmark::State& state) {
    int numRecords = state.range(0);
    PseudoRandom rand(1);
    for (auto _ : state) {
        state.PauseTiming();
        RollingStats stats;
        for (int i = 0; i < numRecords; ++i) {
            stats.record(rand.nextInt64(kMaxMeasurement));
        }
        state.ResumeTiming();
        RollingStatsBmHelper::sumFromWindowData(stats);
    }
}
BENCHMARK(BM_GetStatsLockedTimeSingleTime)->Range(1, 64000)->Iterations(1000);

// Calls the internal function exposed by the helper to track how much time the lock is held when
// computing stats, which should indicate the likelihood of this causing noticeable contention.
void BM_GetStatsLockedTimeDifferentTimes(benchmark::State& state) {
    int numRecords = state.range(0);
    PseudoRandom rand(1);
    ClockSourceMock clock;
    // Spread the records out evenly over 1 min. Need to use Microseconds because eventually
    // numRecords will be greater than 60k, which is the number of millis in a minute.
    Microseconds timeIncrement = Microseconds(Seconds(60)) / numRecords;
    Microseconds elapsedTime(0);

    for (auto _ : state) {
        state.PauseTiming();
        RollingStats stats({.clock = &clock});
        for (int i = 0; i < numRecords; ++i) {
            stats.record(rand.nextInt64(kMaxMeasurement));

            elapsedTime += timeIncrement;
            if (elapsedTime > Milliseconds(1)) {
                Milliseconds elapsedMillis = duration_cast<Milliseconds>(elapsedTime);
                elapsedTime -= elapsedMillis;
                clock.advance(elapsedMillis);
            }
        }
        state.ResumeTiming();
        RollingStatsBmHelper::sumFromWindowData(stats);
    }
}
BENCHMARK(BM_GetStatsLockedTimeDifferentTimes)->Range(1, 64000)->Iterations(1000);

// Calls the internal function exposed by the helper to track how much time the lock is held when
// computing stats, which should indicate the likelihood of this causing noticeable contention.
void BM_GetStatsLockedTime64kDifferentWindowIncrements(benchmark::State& state) {
    Seconds windowIncrement(state.range(0));
    int kNumRecords = 64000;
    PseudoRandom rand(1);
    ClockSourceMock clock;
    // Spread the records out evenly over 1 min. Need to use Microseconds because eventually
    // numRecords will be greater than 60k, which is the number of millis in a minute.
    Microseconds timeIncrement = Microseconds(Seconds(60)) / kNumRecords;
    Microseconds elapsedTime(0);

    for (auto _ : state) {
        state.PauseTiming();
        RollingStats stats({.windowIncrement = windowIncrement, .clock = &clock});
        for (int i = 0; i < kNumRecords; ++i) {
            stats.record(rand.nextInt64(kMaxMeasurement));

            elapsedTime += timeIncrement;
            if (elapsedTime > Milliseconds(1)) {
                Milliseconds elapsedMillis = duration_cast<Milliseconds>(elapsedTime);
                elapsedTime -= elapsedMillis;
                clock.advance(elapsedMillis);
            }
        }
        state.ResumeTiming();
        RollingStatsBmHelper::sumFromWindowData(stats);
    }
}
BENCHMARK(BM_GetStatsLockedTime64kDifferentWindowIncrements)
    ->Arg(1)
    ->Arg(3)
    ->Arg(5)
    ->Arg(10)
    ->Iterations(1000);

}  // namespace
}  // namespace mongo
