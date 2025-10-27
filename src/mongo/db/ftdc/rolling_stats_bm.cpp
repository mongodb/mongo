/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
