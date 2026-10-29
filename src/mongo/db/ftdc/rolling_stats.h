// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/histogram.h"
#include "mongo/util/modules.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/time_support.h"

#include <deque>
#include <mutex>

namespace mongo {
// Basic statistics from the RollingStats class.
struct [[MONGO_MOD_PUBLIC]] RollingStatsResult {
    int64_t count = 0;
    // The average value, based on the bucketed results.
    float mean = 0;
    // The actual observed maximum value. While the mean and percentile stats are based off of
    // approximations, the max is the actual max seen.
    int64_t max = 0;
    // Different percentile values, based on the bucketed results.
    int64_t p50 = 0;
    int64_t p90 = 0;
    int64_t p99 = 0;
};

/**
 * This class returns basic approximate statistics for measurements within a recent time window
 * (~1m). Approximation comes from the fact that the statistics are bucketed so will be slightly
 * smaller (25% at most) then the actual values, and from the fact that we group values 1s time
 * windows rather than record exact times. This is thread safe.
 */
class [[MONGO_MOD_PUBLIC]] RollingStats {
public:
    struct Options {
        // Specifies how recent recorded values need to be to be included in stats.
        Milliseconds windowDuration = Seconds(60);
        // The increment to group recorded values. A larger value means less memory is used
        // recording values, at the cost of stats being more of an approximation (values recorded
        // between windowDuration and windowDuration - windowIncrement may be dropped when computing
        // stats).
        Milliseconds windowIncrement = Seconds(1);
        // The maximum possible recorded value. Since values are bucketed, anything recorded above
        // this value will be in the highest bucket.
        int64_t maxValue = 10'000;
        // Primarily for testing.
        ClockSource* clock = SystemClockSource::get();
    };

    explicit RollingStats(const Options& options);
    RollingStats() : RollingStats(Options()) {};

    // Records the value at now. This should be a nonnegative value for stats to make sense.
    void record(int64_t value);

    // Returns the approximate result as computed in the last minute.
    RollingStatsResult getStats() const;

private:
    friend class RollingStatsBmHelper;
    // The recorded data for a specific time.
    struct DataAtTime {
        Date_t date;
        int64_t max = 0;
        Histogram<int64_t> histogram;
    };
    // Aggregate data within a window.
    struct WindowData {
        int64_t sum = 0;
        int64_t count = 0;
        int64_t max = 0;
        Histogram<int64_t> histogram;
    };

    // Reads the aggregate data for the current window. In particular, collapses all 1s windows into
    // a single Histogram to support computing statistics.
    WindowData _readWindowData() const;

    // Removes histograms that are too old from _window, i.e., that are for a time more than 1m ago.
    void _purgeOldEntries(WithLock, Date_t now);

    const Milliseconds _windowDuration;
    const Milliseconds _windowIncrement;
    // The bucket boundaries for recorded values.
    const std::vector<int64_t> _valuePartitions;

    mutable std::mutex _mutex;
    // Histograms for each 1s time interval.
    std::deque<DataAtTime> _window;
    ClockSource* _clock;
};
}  // namespace mongo
