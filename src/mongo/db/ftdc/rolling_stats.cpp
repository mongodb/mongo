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

namespace mongo {
namespace {
// Returns the partitions used by all histograms.
std::vector<int64_t> createPartitions(const int64_t maxValue) {
    // Increment by 25% until getting to 10k, which results in about 40 buckets.
    // For testing, the initial partition boundaries are:
    // 1, 2, 3, 4, 5, 7, 9, 12, 15, 19, 24, 30, 38, 48, 60, 75, 94, 118
    std::vector<int64_t> partitions;
    int64_t current_value = 1;

    do {
        partitions.push_back(current_value);
        current_value = static_cast<int64_t>(std::ceil(1.25 * current_value));
    } while (current_value < maxValue);

    partitions.push_back(maxValue);
    return partitions;
}

// Returns the numeric value we will use as the value of a point in a Histogram defined by
// partitions with bucket ending at partitionIt, where partitions.end() represents the final bucket.
int64_t currentPartitionValue(auto partitionIt, const std::vector<int64_t>& partitions) {
    if (partitionIt == partitions.begin()) {
        return 0;
    }
    return *(partitionIt - 1);
}

// Returns a numeric value that can be inserted into a Histogram defined by partitions into a bucket
// ending at partitionIt, where partitions.end() represents the final bucket.
int64_t examplePartitionValue(auto partitionIt, const std::vector<int64_t>& partitions) {
    if (partitionIt == partitions.begin()) {
        // Just using *partitionIt will put it in the 2nd bucket, so subtract one to ensure it goes
        // to the previous bucket.
        return *partitionIt - 1;
    }
    return *(partitionIt - 1);
}

// Returns the values for the given percentiles from the given histogram and the total count of
// values in the histogram. percentiles must be in increasing order (e.g. {50, 90, 99}).
std::vector<int64_t> getPercentileValues(const Histogram<int64_t>& histogram,
                                         int64_t totalCount,
                                         const std::vector<int64_t>& percentiles,
                                         const std::vector<int64_t>& partitions) {
    if (percentiles.empty()) {
        return {};
    }

    std::vector<int64_t> values;
    values.reserve(percentiles.size());
    auto percentilesIt = percentiles.begin();
    auto partitionIt = partitions.begin();
    std::vector<int64_t> counts = histogram.getCounts();
    int64_t running_count = 0;
    int64_t percentileIndex = *percentilesIt * totalCount / 100;

    for (auto countIt = counts.begin(); countIt != counts.end(); ++countIt) {
        running_count += *countIt;
        while (running_count >= percentileIndex) {
            values.push_back(currentPartitionValue(partitionIt, partitions));
            ++percentilesIt;
            if (percentilesIt == percentiles.end()) {
                return values;
            }
            percentileIndex = *percentilesIt * totalCount / 100;
        }
        ++partitionIt;
    }

    invariant(!"Never found percentiles.");
    return {};
}
}  // namespace

RollingStats::RollingStats(const RollingStats::Options& options)
    : _windowDuration(options.windowDuration),
      _windowIncrement(options.windowIncrement),
      _valuePartitions(createPartitions(options.maxValue)),
      _clock(options.clock) {}

void RollingStats::_purgeOldEntries(WithLock, Date_t now) {
    while (!_window.empty() && now - _window.front().date > _windowDuration) {
        _window.pop_front();
    }
}

void RollingStats::record(int64_t value) {
    Date_t now = _clock->now();
    stdx::lock_guard lock(_mutex);
    _purgeOldEntries(lock, now);

    if (_window.empty() || now - _window.back().date > _windowIncrement) {
        _window.push_back({.date = _clock->now(), .histogram = Histogram(_valuePartitions)});
    }

    DataAtTime& currentData = _window.back();
    currentData.histogram.increment(value);
    currentData.max = std::max(currentData.max, value);
}

RollingStats::WindowData RollingStats::_readWindowData() const {
    WindowData data{.histogram = Histogram<int64_t>(_valuePartitions)};
    Date_t now = _clock->now();
    {
        stdx::lock_guard lock(_mutex);
        // For each entry in _window, we need to iterate over all the counts present and add them to
        // the aggregate WindowData.
        for (auto windowIt = _window.begin(); windowIt != _window.end(); ++windowIt) {
            const auto& [time, max, histogram] = *windowIt;
            // Skip parts of the window that are too old.
            if (now - time > _windowDuration) {
                continue;
            }

            auto partitionIt = _valuePartitions.begin();
            for (int64_t count : histogram.getCounts()) {
                data.count += count;
                data.max = std::max(data.max, max);
                int64_t value = currentPartitionValue(partitionIt, _valuePartitions);
                data.histogram.incrementN(examplePartitionValue(partitionIt, _valuePartitions),
                                          count);
                data.sum += value * count;
                ++partitionIt;
            }
        }
    }
    return data;
}

RollingStatsResult RollingStats::getStats() const {
    RollingStatsResult result;
    auto [sum, count, max, histogram] = _readWindowData();

    if (count == 0) {
        return result;
    }

    std::vector<int64_t> percentileValues =
        getPercentileValues(histogram, count, /*percentiles=*/{50, 90, 99}, _valuePartitions);
    invariant(percentileValues.size() == 3);

    return {
        .count = count,
        .mean = static_cast<float>(sum / static_cast<double>(count)),
        .max = max,
        .p50 = percentileValues[0],
        .p90 = percentileValues[1],
        .p99 = percentileValues[2],
    };
}

}  // namespace mongo
